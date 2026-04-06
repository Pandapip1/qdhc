/*
 * qdhc — Quick & Dirty Haskell Compiler
 *
 * Haskell subset -> native object file via tree-sitter + LLVM C API.
 * Presents a GCC-compatible driver interface so that CMake (and any other
 * build system that drives a C compiler) can use qdhc as a drop-in CC:
 *
 *   cmake -DCMAKE_C_COMPILER=/path/to/qdhc ...
 *
 * Driver behaviour:
 *   • Input is a .hs file  → compile Haskell to a native .o (or executable)
 *   • Input is a .c file, or -x c/c++ is set, or it's link-only, or -E →
 *     delegate transparently to $QDHC_CC (default: cc) with all original args
 *   • --version / -v       → print a GHC-flavoured version string and exit 0
 *
 * GCC-style flags recognised:
 *   -c            compile only (produce .o, no link)
 *   -o <file>     output path
 *   -x <lang>     language override; non-haskell langs delegate to cc
 *   -I<dir>       include path (accepted, ignored for Haskell)
 *   -D<macro>     define     (accepted, ignored for Haskell)
 *   -W...         warning    (accepted, ignored)
 *   -O...         optimise   (accepted, ignored)
 *   -std=...      standard   (accepted, ignored)
 *   -f...         feature    (accepted, ignored)
 *   -g            debug info (accepted, ignored)
 *   -shared/-fPIC (accepted, ignored — qdhc produces static objects)
 *   -M/-MM/-MF/-MD/-MDD  dependency tracking (delegate to cc)
 *   -E            preprocess only (delegate to cc)
 *   -S            assemble only (compile Haskell → .ll text as .s stand-in)
 *   --run         (legacy) JIT-compile and execute in-process
 *   --dump-dir D  (legacy/debug) write cst.txt + ir.ll to D
 *
 * Supported Haskell subset:
 *   - Integer literals and arithmetic (+, -, *, div, mod, quot, rem)
 *   - Comparisons (==, /=, <, <=, >, >=) and boolean (&&, ||)
 *   - Top-level function definitions with multiple parameters
 *   - if/then/else (phi-node based SSA)
 *   - let-in expressions and do-let bindings
 *   - Function application (curried, multi-arg)
 *   - Recursion
 *   - Type signatures (parsed, ignored for codegen)
 *   - C main() wrapper that calls user's `main`, prints its Int result
 *
 * Key AST facts (tree-sitter-haskell grammar):
 *   function: name=variable, patterns=patterns{var...}, binds=match
 *   match:    expression=<body expr>
 *   bind:     name=variable, binds=match
 *   do:       statement=(let|exp|bind)*
 *   let:      binds=local_binds{bind...}
 *   apply:    function=<callee|apply>, argument=<arg>   (curried left-assoc)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>   /* execvp, fork */

#include <tree_sitter/api.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/ExecutionEngine.h>

extern const TSLanguage *tree_sitter_haskell(void);

/* ── constants ──────────────────────────────────────────────────────────── */
#define MAX_NAME   256
#define MAX_ARGS   16
#define MAX_FUNCS  256
#define MAX_LOCALS 512

static void die(const char *m) { fprintf(stderr,"fatal: %s\n",m); exit(1); }

/* ── node helpers ───────────────────────────────────────────────────────── */

static char *node_text(TSNode n, const char *src) {
    uint32_t s=ts_node_start_byte(n), e=ts_node_end_byte(n);
    char *b=malloc(e-s+1); memcpy(b,src+s,e-s); b[e-s]=0; return b;
}
static bool nn(TSNode n)             { return ts_node_is_null(n); }
static const char *nt(TSNode n)      { return ts_node_type(n); }
static bool named(TSNode n)          { return ts_node_is_named(n); }

/* Return child with given field name, or null node. */
static TSNode fchild(TSNode node, const char *field) {
    /* tree-sitter 0.20.8 + tree-sitter-haskell quirk:
       ts_node_child_by_field_name() is broken for this grammar.
       ts_node_field_name_for_child() correctly labels children, BUT
       for the root `haskell` node a leading comment can steal the
       `declarations` field label — the real declarations node then
       appears immediately after with no field label.

       Strategy: scan for the field label; if the child at that index
       is a comment (or otherwise not the right kind), keep walking
       forward until we hit the first child whose type matches the
       field name (e.g. field "declarations" -> type "declarations",
       field "match" -> type "match" (function/bind body),
       field "binds" -> type "local_binds" (where-clause bindings).
       For all other fields the labeled child IS the right node. */
    uint32_t nc = ts_node_child_count(node);
    for(uint32_t i = 0; i < nc; i++){
        const char *fn = ts_node_field_name_for_child(node, i);
        if(!fn || strcmp(fn, field) != 0) continue;
        TSNode c = ts_node_child(node, i);
        /* If the labeled child is a comment, scan forward for the
           first named non-comment child. */
        if(!strcmp(ts_node_type(c), "comment")){
            for(uint32_t j = i+1; j < nc; j++){
                TSNode sib = ts_node_child(node, j);
                if(ts_node_is_named(sib) && strcmp(ts_node_type(sib),"comment")!=0)
                    return sib;
            }
        }
        return c;
    }
    TSNode z; memset(&z, 0, sizeof z); return z;
}

/* First named child. */
static TSNode fnc(TSNode node) {
    uint32_t nc=ts_node_child_count(node);
    for(uint32_t i=0;i<nc;i++){
        TSNode c=ts_node_child(node,i);
        if(named(c)) return c;
    }
    TSNode z; memset(&z,0,sizeof z); return z;
}

/* First named child with given type. */
static TSNode fncT(TSNode node, const char *type) {
    uint32_t nc=ts_node_child_count(node);
    for(uint32_t i=0;i<nc;i++){
        TSNode c=ts_node_child(node,i);
        if(named(c)&&!strcmp(nt(c),type)) return c;
    }
    TSNode z; memset(&z,0,sizeof z); return z;
}

/* Body expression from a `match` node (field "expression"). */
static TSNode match_body(TSNode match) { return fchild(match,"expression"); }

/* Debug dump — writes the CST to `out` (pass stderr or a file) */
static void dump(TSNode n, const char *src, int d, FILE *out) {
    if(nn(n)) return;
    for(int i=0;i<d;i++) fprintf(out,"  ");
    uint32_t s=ts_node_start_byte(n),e=ts_node_end_byte(n);
    int l=(int)(e-s); if(l>50)l=50;
    char snip[64]={0}; memcpy(snip,src+s,l);
    fprintf(out,"[%s%s] \"%s\"\n", named(n)?"":"~", nt(n), snip);
    uint32_t nc=ts_node_child_count(n);
    for(uint32_t i=0;i<nc;i++){
        const char *fn=ts_node_field_name_for_child(n,i);
        if(fn){ for(int j=0;j<d+1;j++) fprintf(out,"  "); fprintf(out,"(field=%s)\n",fn); }
        dump(ts_node_child(n,i),src,d+1,out);
    }
}

/* ── symbol table ───────────────────────────────────────────────────────── */

typedef struct { char name[MAX_NAME]; int arity; LLVMValueRef ref; } Func;
typedef struct { char name[MAX_NAME]; LLVMValueRef val; bool is_ptr; } Local;
typedef struct {
    Func  funcs[MAX_FUNCS]; int nfuncs;
    Local locals[MAX_LOCALS]; int nlocals;
} Sym;
static Sym sym;

static Func *sym_func(const char *n) {
    for(int i=0;i<sym.nfuncs;i++)
        if(!strcmp(sym.funcs[i].name,n)) return &sym.funcs[i];
    return NULL;
}
static Func *sym_add(const char *n, int arity) {
    if(sym.nfuncs>=MAX_FUNCS) die("too many funcs");
    Func *f=&sym.funcs[sym.nfuncs++];
    strncpy(f->name,n,MAX_NAME-1); f->arity=arity; f->ref=NULL; return f;
}
static void push_local(const char *n, LLVMValueRef v, bool ptr) {
    if(sym.nlocals>=MAX_LOCALS) die("too many locals");
    Local *l=&sym.locals[sym.nlocals++];
    strncpy(l->name,n,MAX_NAME-1); l->val=v; l->is_ptr=ptr;
}
static LLVMValueRef get_local(const char *n, bool *ptr_out) {
    for(int i=sym.nlocals-1;i>=0;i--)
        if(!strcmp(sym.locals[i].name,n)){
            if(ptr_out) *ptr_out=sym.locals[i].is_ptr;
            return sym.locals[i].val;
        }
    return NULL;
}
static int  save_locals(void)       { return sym.nlocals; }
static void rest_locals(int s)      { sym.nlocals=s; }

/* ── codegen state ──────────────────────────────────────────────────────── */

typedef struct {
    LLVMContextRef ctx;
    LLVMModuleRef  mod;
    LLVMBuilderRef b;
    LLVMTypeRef    i64, i32;
    const char    *src;
} CG;

static LLVMValueRef cg_expr(CG *cg, TSNode node); /* forward */

static LLVMTypeRef ftype(CG *cg, int arity) {
    LLVMTypeRef *p=malloc(sizeof(LLVMTypeRef)*(arity?arity:1));
    for(int i=0;i<arity;i++) p[i]=cg->i64;
    LLVMTypeRef t=LLVMFunctionType(cg->i64,p,arity,0);
    free(p); return t;
}

/* Alloca in the function entry block. */
static LLVMValueRef entry_alloca(CG *cg, const char *name) {
    LLVMValueRef fn=LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->b));
    LLVMBasicBlockRef ebb=LLVMGetEntryBasicBlock(fn);
    LLVMBuilderRef eb=LLVMCreateBuilderInContext(cg->ctx);
    LLVMValueRef fi=LLVMGetFirstInstruction(ebb);
    if(fi) LLVMPositionBuilderBefore(eb,fi); else LLVMPositionBuilderAtEnd(eb,ebb);
    LLVMValueRef a=LLVMBuildAlloca(eb,cg->i64,name);
    LLVMDisposeBuilder(eb); return a;
}

/* ── expression codegen ─────────────────────────────────────────────────── */

static LLVMValueRef cg_int(CG *cg, TSNode n) {
    char *t=node_text(n,cg->src);
    long long v=strtoll(t,NULL,10); free(t);
    return LLVMConstInt(cg->i64,(unsigned long long)v,1);
}

static LLVMValueRef cg_var(CG *cg, TSNode n) {
    char *name=node_text(n,cg->src);
    bool ptr=false;
    LLVMValueRef loc=get_local(name,&ptr);
    if(loc){ free(name);
        return ptr ? LLVMBuildLoad2(cg->b,cg->i64,loc,"ld") : loc; }
    Func *f=sym_func(name);
    if(f&&f->ref&&f->arity==0){ free(name);
        return LLVMBuildCall2(cg->b,ftype(cg,0),f->ref,NULL,0,"call0"); }
    fprintf(stderr,"error: unsupported or unknown variable '%s'\n",name); free(name);
    exit(1);
}

static LLVMValueRef cg_infix(CG *cg, TSNode n) {
    TSNode ln=fchild(n,"left_operand"), opn=fchild(n,"operator"), rn=fchild(n,"right_operand");
    /* Grammar quirk: inside parens, `n - 1` omits left_operand/right_operand field tags.
       Children are positional: [0]=LHS [1]=op(no field) [2]=RHS.
       Also the named `operator` field sometimes points to the RHS literal, not the op symbol.
       Detect this and fall back to positional scanning. */
    if(nn(ln)||nn(rn)){
        uint32_t nc=ts_node_child_count(n);
        if(nc>=3){ ln=ts_node_child(n,0); opn=ts_node_child(n,1); rn=ts_node_child(n,2); }
    } else if(!nn(opn)){
        /* operator field may point to the RHS literal instead of the symbol;
           the real symbol is child[1] with no field tag */
        const char *opt=ts_node_type(opn);
        if(strcmp(opt,"operator")!=0){
            /* real op is the unnamed child between LHS and RHS */
            uint32_t nc=ts_node_child_count(n);
            for(uint32_t i=0;i<nc;i++){
                const char *fn=ts_node_field_name_for_child(n,i);
                if(!fn) { opn=ts_node_child(n,i); break; }
            }
        }
    }
    if(nn(ln)||nn(rn)){
        fprintf(stderr,"warning: infix missing operand\n");
        return LLVMConstInt(cg->i64,0,0);
    }
    LLVMValueRef l=cg_expr(cg,ln), r=cg_expr(cg,rn);
    char *op=nn(opn)?strdup("?"):node_text(opn,cg->src);
    LLVMValueRef res;
    if     (!strcmp(op,"+"))  res=LLVMBuildAdd (cg->b,l,r,"add");
    else if(!strcmp(op,"-"))  res=LLVMBuildSub (cg->b,l,r,"sub");
    else if(!strcmp(op,"*"))  res=LLVMBuildMul (cg->b,l,r,"mul");
    else if(!strcmp(op,"/"))  res=LLVMBuildSDiv(cg->b,l,r,"div");
    else if(!strcmp(op,"div")||!strcmp(op,"`div`")) res=LLVMBuildSDiv(cg->b,l,r,"div");
    else if(!strcmp(op,"mod")||!strcmp(op,"`mod`")) res=LLVMBuildSRem(cg->b,l,r,"mod");
    else if(!strcmp(op,"quot")||!strcmp(op,"`quot`")) res=LLVMBuildSDiv(cg->b,l,r,"quot");
    else if(!strcmp(op,"rem")||!strcmp(op,"`rem`")) res=LLVMBuildSRem(cg->b,l,r,"rem");
    else if(!strcmp(op,"==")){ LLVMValueRef c=LLVMBuildICmp(cg->b,LLVMIntEQ, l,r,"eq"); res=LLVMBuildZExt(cg->b,c,cg->i64,"eqi"); }
    else if(!strcmp(op,"/=")){ LLVMValueRef c=LLVMBuildICmp(cg->b,LLVMIntNE, l,r,"ne"); res=LLVMBuildZExt(cg->b,c,cg->i64,"nei"); }
    else if(!strcmp(op,"<")) { LLVMValueRef c=LLVMBuildICmp(cg->b,LLVMIntSLT,l,r,"lt"); res=LLVMBuildZExt(cg->b,c,cg->i64,"lti"); }
    else if(!strcmp(op,"<=")){ LLVMValueRef c=LLVMBuildICmp(cg->b,LLVMIntSLE,l,r,"le"); res=LLVMBuildZExt(cg->b,c,cg->i64,"lei"); }
    else if(!strcmp(op,">")) { LLVMValueRef c=LLVMBuildICmp(cg->b,LLVMIntSGT,l,r,"gt"); res=LLVMBuildZExt(cg->b,c,cg->i64,"gti"); }
    else if(!strcmp(op,">=")){ LLVMValueRef c=LLVMBuildICmp(cg->b,LLVMIntSGE,l,r,"ge"); res=LLVMBuildZExt(cg->b,c,cg->i64,"gei"); }
    else if(!strcmp(op,"&&")) res=LLVMBuildAnd(cg->b,l,r,"and");
    else if(!strcmp(op,"||")) res=LLVMBuildOr (cg->b,l,r,"or");
    else { fprintf(stderr,"error: unsupported operator '%s'\n",op);
           free(op); exit(1); }
    free(op); return res;
}

static LLVMValueRef cg_if(CG *cg, TSNode n) {
    LLVMValueRef cv=cg_expr(cg,fchild(n,"if"));
    LLVMValueRef zero=LLVMConstInt(cg->i64,0,0);
    LLVMValueRef ci1=LLVMBuildICmp(cg->b,LLVMIntNE,cv,zero,"cond");
    LLVMValueRef fn=LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->b));
    LLVMBasicBlockRef tbb=LLVMAppendBasicBlockInContext(cg->ctx,fn,"then");
    LLVMBasicBlockRef ebb=LLVMAppendBasicBlockInContext(cg->ctx,fn,"else");
    LLVMBasicBlockRef mbb=LLVMAppendBasicBlockInContext(cg->ctx,fn,"merge");
    LLVMBuildCondBr(cg->b,ci1,tbb,ebb);

    LLVMPositionBuilderAtEnd(cg->b,tbb);
    LLVMValueRef tv=cg_expr(cg,fchild(n,"then"));
    LLVMBuildBr(cg->b,mbb); LLVMBasicBlockRef te=LLVMGetInsertBlock(cg->b);

    LLVMPositionBuilderAtEnd(cg->b,ebb);
    LLVMValueRef ev=cg_expr(cg,fchild(n,"else"));
    LLVMBuildBr(cg->b,mbb); LLVMBasicBlockRef ee=LLVMGetInsertBlock(cg->b);

    LLVMPositionBuilderAtEnd(cg->b,mbb);
    LLVMValueRef phi=LLVMBuildPhi(cg->b,cg->i64,"ifv");
    LLVMAddIncoming(phi,&tv,&te,1); LLVMAddIncoming(phi,&ev,&ee,1);
    return phi;
}

/* Recursively unwrap curried apply: apply(apply(f,a),b) -> callee=f, args=[a,b] */
/* Collect callee node + raw argument nodes (no codegen yet) so we can
   evaluate them left-to-right after fully unwinding the curried apply chain.
   apply(apply(f,a),b)  ->  callee=f, arg_nodes=[a, b]  */
static void collect_apply_nodes(TSNode n, TSNode *arg_nodes, int *na, TSNode *callee) {
    TSNode fn=fchild(n,"function"), an=fchild(n,"argument");
    if(!nn(fn)){
        if(!strcmp(nt(fn),"apply")) collect_apply_nodes(fn,arg_nodes,na,callee);
        else *callee=fn;
    }
    if(!nn(an) && *na<MAX_ARGS) arg_nodes[(*na)++]=an;
}

static LLVMValueRef cg_apply(CG *cg, TSNode n) {
    TSNode arg_nodes[MAX_ARGS]; int na=0;
    TSNode callee; memset(&callee,0,sizeof callee);
    collect_apply_nodes(n,arg_nodes,&na,&callee);
    if(nn(callee)){ fprintf(stderr,"warning: apply no callee\n"); return LLVMConstInt(cg->i64,0,0); }
    char *fn=node_text(callee,cg->src);
    Func *fe=sym_func(fn);
    if(!fe||!fe->ref){ fprintf(stderr,"error: unsupported or unknown function '%s'\n",fn); free(fn); exit(1); }
    free(fn);
    /* Now evaluate args in order, left to right */
    int ca=na<fe->arity?na:fe->arity;
    LLVMValueRef args[MAX_ARGS];
    for(int i=0;i<ca;i++) args[i]=cg_expr(cg,arg_nodes[i]);
    return LLVMBuildCall2(cg->b,ftype(cg,fe->arity),fe->ref,args,ca,"app");
}

/* Emit a bind: name = rhs (via alloca).
   The rhs match node is in field "match" per ts_node_field_name_for_child. */
static void emit_bind(CG *cg, TSNode bnd) {
    TSNode nn_=fchild(bnd,"name"), mn=fchild(bnd,"match");
    if(nn(nn_)||nn(mn)) return;
    TSNode body=match_body(mn);
    if(nn(body)) return;
    char *bname=node_text(nn_,cg->src);
    LLVMValueRef rhs=cg_expr(cg,body);
    LLVMValueRef al=entry_alloca(cg,bname);
    LLVMBuildStore(cg->b,rhs,al);
    push_local(bname,al,true);
    free(bname);
}

static LLVMValueRef cg_letin(CG *cg, TSNode n) {
    int sv=save_locals();
    TSNode lb=fchild(n,"binds"); /* local_binds */
    if(!nn(lb)){
        uint32_t nc=ts_node_child_count(lb);
        for(uint32_t i=0;i<nc;i++){
            TSNode c=ts_node_child(lb,i);
            if(!named(c)) continue;
            if(!strcmp(nt(c),"bind")||!strcmp(nt(c),"function")) emit_bind(cg,c);
        }
    }
    TSNode body=fchild(n,"expression");
    LLVMValueRef r=nn(body)?LLVMConstInt(cg->i64,0,0):cg_expr(cg,body);
    rest_locals(sv); return r;
}

static LLVMValueRef cg_do(CG *cg, TSNode n) {
    int sv=save_locals();
    LLVMValueRef last=LLVMConstInt(cg->i64,0,0);
    uint32_t nc=ts_node_child_count(n);
    for(uint32_t i=0;i<nc;i++){
        TSNode c=ts_node_child(n,i);
        if(!named(c)) continue;
        const char *ct=nt(c);
        if(!strcmp(ct,"let")){
            TSNode lb=fchild(c,"binds");
            if(!nn(lb)){
                uint32_t nb=ts_node_child_count(lb);
                for(uint32_t j=0;j<nb;j++){
                    TSNode b=ts_node_child(lb,j);
                    if(!named(b)) continue;
                    if(!strcmp(nt(b),"bind")||!strcmp(nt(b),"function")) emit_bind(cg,b);
                }
            }
        } else if(!strcmp(ct,"exp")||!strcmp(ct,"expression")){
            TSNode e=fnc(c);
            if(!nn(e)) last=cg_expr(cg,e);
        } else {
            /* anything else: just evaluate */
            last=cg_expr(cg,c);
        }
    }
    rest_locals(sv); return last;
}

static LLVMValueRef cg_neg(CG *cg, TSNode n) {
    TSNode e=fchild(n,"expression");
    if(nn(e)) e=fchild(n,"number");
    if(nn(e)) return LLVMConstInt(cg->i64,0,0);
    return LLVMBuildNeg(cg->b,cg_expr(cg,e),"neg");
}

static LLVMValueRef cg_expr(CG *cg, TSNode n) {
    if(nn(n)) return LLVMConstInt(cg->i64,0,0);
    const char *t=nt(n);

    /* Transparent wrappers */
    if(!strcmp(t,"exp")||!strcmp(t,"expression")||!strcmp(t,"literal")||!strcmp(t,"parens")){
        TSNode c=fnc(n); return nn(c)?LLVMConstInt(cg->i64,0,0):cg_expr(cg,c);
    }
    /* match node -> descend to body */
    if(!strcmp(t,"match")){
        TSNode body=match_body(n);
        return nn(body)?LLVMConstInt(cg->i64,0,0):cg_expr(cg,body);
    }

    if(!strcmp(t,"integer"))     return cg_int(cg,n);
    if(!strcmp(t,"variable"))    return cg_var(cg,n);
    if(!strcmp(t,"infix"))       return cg_infix(cg,n);
    if(!strcmp(t,"conditional")) return cg_if(cg,n);
    if(!strcmp(t,"apply"))       return cg_apply(cg,n);
    if(!strcmp(t,"let_in"))      return cg_letin(cg,n);
    if(!strcmp(t,"do"))          return cg_do(cg,n);
    if(!strcmp(t,"negation"))    return cg_neg(cg,n);
    if(!strcmp(t,"constructor")){
        char *tx=node_text(n,cg->src);
        long long v=!strcmp(tx,"True")?1:0; free(tx);
        return LLVMConstInt(cg->i64,v,0);
    }
    fprintf(stderr,"error: unsupported expression type '%s'\n",t);
    exit(1);
}

/* ── top-level function codegen ─────────────────────────────────────────── */

static void cg_function(CG *cg, TSNode node) {
    TSNode name_nd=fchild(node,"name");
    if(nn(name_nd)) return;
    char *fname=node_text(name_nd,cg->src);
    Func *fe=sym_func(fname); free(fname);
    if(!fe||!fe->ref) return;
    if(LLVMCountBasicBlocks(fe->ref)>0) return; /* already emitted */

    /* Collect param names from patterns node children */
    char pnames[MAX_ARGS][MAX_NAME]; int arity=0;
    TSNode pat=fchild(node,"patterns");
    if(!nn(pat)){
        uint32_t np=ts_node_child_count(pat);
        for(uint32_t i=0;i<np&&arity<MAX_ARGS;i++){
            TSNode p=ts_node_child(pat,i);
            if(!named(p)) continue;
            char *pn=node_text(p,cg->src);
            strncpy(pnames[arity++],pn,MAX_NAME-1); free(pn);
        }
    }

    LLVMBasicBlockRef bb=LLVMAppendBasicBlockInContext(cg->ctx,fe->ref,"entry");
    LLVMPositionBuilderAtEnd(cg->b,bb);

    int sv=save_locals();
    for(int i=0;i<arity;i++){
        LLVMValueRef p=LLVMGetParam(fe->ref,i);
        LLVMSetValueName2(p,pnames[i],strlen(pnames[i]));
        push_local(pnames[i],p,false);
    }

    /* The function body is in field "match" (a match node). This is what
       ts_node_field_name_for_child correctly reports for visible child[2],
       which is the match node containing the rhs expression. Note that
       ts_node_child_by_field_name("match") returns NULL because the grammar
       marks this as inherited (pointing to a where-clause); we use our manual
       fchild() which uses ts_node_field_name_for_child instead. */
    TSNode match=fchild(node,"match");
    if(!nn(match)){
        /* Detect guards: match has a 'guards' child */
        TSNode guards=fchild(match,"guards");
        if(!nn(guards)){
            char *fname2=node_text(fchild(node,"name"),cg->src);
            fprintf(stderr,"error: unsupported: guards in function '%s'\n", fname2);
            free(fname2); exit(1);
        }
        /* Detect where clause */
        TSNode wh=fchild(match,"where");
        if(!nn(wh)){
            char *fname2=node_text(fchild(node,"name"),cg->src);
            fprintf(stderr,"error: unsupported: where clause in function '%s'\n", fname2);
            free(fname2); exit(1);
        }
        /* Detect constructor/tuple patterns in function head */
        if(!nn(fchild(node,"patterns"))){
            TSNode pat=fchild(node,"patterns");
            uint32_t np=ts_node_child_count(pat);
            for(uint32_t i=0;i<np;i++){
                TSNode p=ts_node_child(pat,i);
                if(!named(p)) continue;
                const char *pt=nt(p);
                if(strcmp(pt,"variable")!=0){
                    char *fname2=node_text(fchild(node,"name"),cg->src);
                    fprintf(stderr,"error: unsupported: pattern '%s' in function '%s'\n", pt, fname2);
                    free(fname2); exit(1);
                }
            }
        }
    }
    LLVMValueRef rv=nn(match)?LLVMConstInt(cg->i64,0,0):cg_expr(cg,match);
    LLVMBuildRet(cg->b,rv);
    rest_locals(sv);
}

static void cg_bind_toplevel(CG *cg, TSNode node) {
    TSNode name_nd=fchild(node,"name");
    if(nn(name_nd)||strcmp(nt(name_nd),"variable")) return;
    char *fname=node_text(name_nd,cg->src);
    Func *fe=sym_func(fname); free(fname);
    if(!fe||!fe->ref) return;
    if(LLVMCountBasicBlocks(fe->ref)>0) return;

    LLVMBasicBlockRef bb=LLVMAppendBasicBlockInContext(cg->ctx,fe->ref,"entry");
    LLVMPositionBuilderAtEnd(cg->b,bb);
    /* bind body is in field "match" per ts_node_field_name_for_child */
    TSNode match=fchild(node,"match");
    LLVMValueRef rv=nn(match)?LLVMConstInt(cg->i64,0,0):cg_expr(cg,match);
    LLVMBuildRet(cg->b,rv);
}

/* ── passes ─────────────────────────────────────────────────────────────── */

static TSNode get_decls(TSNode root) {
    /* Try field access first, then named-child search */
    TSNode d=fchild(root,"declarations");
    if(!nn(d)) return d;
    return fncT(root,"declarations");
}

static void first_pass(TSNode root, const char *src) {
    TSNode decls=get_decls(root);
    if(nn(decls)) return;
    uint32_t n=ts_node_child_count(decls);
    for(uint32_t i=0;i<n;i++){
        TSNode d=ts_node_child(decls,i);
        if(!named(d)) continue;
        const char *t=nt(d);
        if(!strcmp(t,"function")){
            TSNode nm=fchild(d,"name"); if(nn(nm)) continue;
            char *fn=node_text(nm,src);
            if(!sym_func(fn)){
                /* Count params: children of patterns node */
                int ar=0;
                TSNode pat=fchild(d,"patterns");
                if(!nn(pat)){
                    uint32_t np=ts_node_child_count(pat);
                    for(uint32_t j=0;j<np;j++) if(named(ts_node_child(pat,j))) ar++;
                }
                sym_add(fn,ar);
            }
            free(fn);
        } else if(!strcmp(t,"bind")){
            TSNode nm=fchild(d,"name"); if(nn(nm)) continue;
            if(strcmp(nt(nm),"variable")) continue;
            char *fn=node_text(nm,src);
            if(!sym_func(fn)) sym_add(fn,0);
            free(fn);
        }
    }
}

static void predeclare(CG *cg) {
    for(int i=0;i<sym.nfuncs;i++){
        Func *f=&sym.funcs[i];
        /* rename user's main -> haskell_main to avoid clash with C main */
        const char *llname=!strcmp(f->name,"main")?"haskell_main":f->name;
        f->ref=LLVMAddFunction(cg->mod,llname,ftype(cg,f->arity));
    }
}

static void second_pass(CG *cg, TSNode root) {
    TSNode decls=get_decls(root);
    if(nn(decls)) return;
    uint32_t n=ts_node_child_count(decls);
    for(uint32_t i=0;i<n;i++){
        TSNode d=ts_node_child(decls,i);
        if(!named(d)) continue;
        const char *dt=nt(d);
        if(!strcmp(dt,"function")) cg_function(cg,d);
        else if(!strcmp(dt,"bind")) cg_bind_toplevel(cg,d);
        else if(!strcmp(dt,"signature")) continue; /* type sigs ignored */
        else if(!strcmp(dt,"comment"))   continue;
        else if(!strcmp(dt,"data_type")||!strcmp(dt,"newtype")){
            /* extract name if possible */
            TSNode nm=fchild(d,"name");
            char *dn=nn(nm)?strdup("?"):node_text(nm,cg->src);
            fprintf(stderr,"error: unsupported: data/newtype declaration '%s'\n", dn);
            free(dn); exit(1);
        } else if(!strcmp(dt,"class")||!strcmp(dt,"instance")){
            fprintf(stderr,"error: unsupported: %s declaration\n", dt);
            exit(1);
        } else if(!strcmp(dt,"default")||!strcmp(dt,"foreign")){
            fprintf(stderr,"error: unsupported: %s declaration\n", dt);
            exit(1);
        } else {
            fprintf(stderr,"error: unsupported top-level declaration type '%s'\n", dt);
            exit(1);
        }
    }
}

static void emit_main(CG *cg) {
    LLVMTypeRef i8ptr=LLVMPointerType(LLVMInt8TypeInContext(cg->ctx),0);
    LLVMTypeRef pp[]={i8ptr};
    LLVMTypeRef pfty=LLVMFunctionType(cg->i32,pp,1,1);
    LLVMValueRef pf=LLVMAddFunction(cg->mod,"printf",pfty);

    LLVMTypeRef mfty=LLVMFunctionType(cg->i32,NULL,0,0);
    LLVMValueRef mf=LLVMAddFunction(cg->mod,"main",mfty);
    LLVMBasicBlockRef bb=LLVMAppendBasicBlockInContext(cg->ctx,mf,"entry");
    LLVMPositionBuilderAtEnd(cg->b,bb);

    Func *hm=sym_func("main");
    LLVMValueRef res=LLVMConstInt(cg->i64,0,0);
    if(hm&&hm->ref)
        res=LLVMBuildCall2(cg->b,ftype(cg,0),hm->ref,NULL,0,"hm");

    LLVMValueRef fmt=LLVMBuildGlobalStringPtr(cg->b,"%lld\n","fmt");
    LLVMValueRef pa[]={fmt,res};
    LLVMBuildCall2(cg->b,pfty,pf,pa,2,"");
    LLVMBuildRet(cg->b,LLVMConstInt(cg->i32,0,0));
}

/* ── CC-driver helpers ──────────────────────────────────────────────────── */

/* Delegate all original argv to $QDHC_CC (default "cc") and exec it.
   Never returns on success. */
static void delegate_to_cc(int argc, char **argv) {
    const char *cc = getenv("QDHC_CC");
    if (!cc || !*cc) cc = "cc";
    /* Build new argv: cc arg1 arg2 ... */
    char **nargv = malloc(sizeof(char*) * (argc + 1));
    nargv[0] = (char*)cc;
    for (int i = 1; i < argc; i++) nargv[i] = argv[i];
    nargv[argc] = NULL;
    execvp(cc, nargv);
    perror(cc);
    exit(1);
}

/* Return true if path has extension ext (e.g. ".hs") */
static bool has_ext(const char *path, const char *ext) {
    size_t pl = strlen(path), el = strlen(ext);
    if (pl < el) return false;
    return strcmp(path + pl - el, ext) == 0;
}

/* Replace or append extension.  Caller must free result. */
static char *replace_ext(const char *path, const char *newext) {
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    size_t base = (dot && (!slash || dot > slash)) ? (size_t)(dot - path) : strlen(path);
    char *out = malloc(base + strlen(newext) + 1);
    memcpy(out, path, base);
    strcpy(out + base, newext);
    return out;
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    /* ── parse GCC-style flags ─────────────────────────────────────────── */
    bool do_compile_only = false;   /* -c */
    bool do_assemble_only = false;  /* -S */
    bool do_preprocess_only = false;/* -E */
    bool do_run = false;            /* --run  (legacy) */
    const char *flag_output = NULL; /* -o <file> */
    const char *flag_x = NULL;      /* -x <lang> */
    const char *dump_dir = NULL;    /* --dump-dir <dir> (legacy/debug) */

    /* Collect non-flag positional inputs (source files, .o files, -l flags) */
    const char *inputs[256]; int ninputs = 0;
    const char *hs_input = NULL;    /* the single .hs source, if any */
    bool has_non_hs_source = false; /* .c / .cpp / etc. */

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];

        /* ── version / help ── */
        if (!strcmp(a, "--version") || !strcmp(a, "-dumpversion")) {
            printf("qdhc 0.1.0 (Quick & Dirty Haskell Compiler, GHC-subset)\n");
            return 0;
        }
        if (!strcmp(a, "-dumpmachine")) {
            /* Ask cc what its machine triple is */
            FILE *p = popen("cc -dumpmachine 2>/dev/null", "r");
            if (p) { char buf[128]={0}; fread(buf,1,127,p); pclose(p);
                     char *nl=strchr(buf,'\n'); if(nl)*nl=0;
                     if(*buf){ puts(buf); return 0; } }
            puts("x86_64-linux-gnu"); return 0;
        }
        if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
            fprintf(stderr, "qdhc 0.1.0 (Quick & Dirty Haskell Compiler)\n");
            /* don't exit — -v is also passed during normal compilation */
            continue;
        }

        /* ── flags that require us to delegate everything to cc ── */
        if (!strcmp(a, "-E")) { do_preprocess_only = true; continue; }
        if (!strcmp(a, "-M") || !strcmp(a, "-MM") || !strcmp(a, "-MG")) {
            delegate_to_cc(argc, argv); /* never returns */
        }

        /* ── flags we consume ── */
        if (!strcmp(a, "-c"))           { do_compile_only = true; continue; }
        if (!strcmp(a, "-S"))           { do_assemble_only = true; continue; }
        if (!strcmp(a, "--run"))        { do_run = true; continue; }
        if (!strcmp(a, "--dump-dir") && i+1 < argc) { dump_dir = argv[++i]; continue; }

        if (!strcmp(a, "-o") && i+1 < argc) { flag_output = argv[++i]; continue; }
        if (!strncmp(a, "-o", 2) && a[2]) { flag_output = a+2; continue; }

        if (!strcmp(a, "-x") && i+1 < argc) { flag_x = argv[++i]; continue; }
        if (!strncmp(a, "-x", 2) && a[2])   { flag_x = a+2; continue; }

        /* ── flags we silently accept (irrelevant for Haskell codegen) ── */
        if (!strncmp(a,"-I",2) || !strncmp(a,"-D",2) || !strncmp(a,"-W",2) ||
            !strncmp(a,"-O",2) || !strncmp(a,"-f",2) || !strncmp(a,"-m",2) ||
            !strncmp(a,"-std=",5) || !strcmp(a,"-g") || !strcmp(a,"-shared") ||
            !strcmp(a,"-fPIC") || !strcmp(a,"-fpic") || !strcmp(a,"-pipe") ||
            !strncmp(a,"-MF",3) || !strncmp(a,"-MT",3) || !strncmp(a,"-MQ",3) ||
            !strcmp(a,"-MD") || !strcmp(a,"-MMD") || !strcmp(a,"-MP") ||
            !strcmp(a,"-pthread") || !strncmp(a,"--sysroot",9) ||
            !strncmp(a,"-isystem",8) || !strncmp(a,"-isysroot",9) ||
            !strncmp(a,"-target",7) || !strncmp(a,"--target",8))
        {
            continue;
        }
        /* -l<lib> and -L<dir>: linker flags, collected but ignored for .hs compiles */
        if (!strncmp(a,"-l",2) || !strncmp(a,"-L",2) || !strcmp(a,"-rdynamic") ||
            !strncmp(a,"-Wl,",4) || !strncmp(a,"-Xlinker",8))
        { continue; }

        /* ── positional: source / object inputs ── */
        if (a[0] != '-') {
            if (ninputs < 256) inputs[ninputs++] = a;
            if (has_ext(a, ".hs")) {
                hs_input = a;
            } else if (!has_ext(a, ".o") && !has_ext(a, ".a") &&
                       !has_ext(a, ".so") && !has_ext(a, ".dylib")) {
                has_non_hs_source = true;
            }
            continue;
        }

        /* Unknown flag: pass through (will reach cc if we delegate) */
    }

    /* ── decide dispatch ──────────────────────────────────────────────── */

    /* -x overrides language: if it's not haskell, delegate */
    bool x_is_haskell = !flag_x ||
                        !strcmp(flag_x,"haskell") ||
                        !strcmp(flag_x,"Haskell");

    /* If the language is forced to non-haskell, or we have non-.hs source
       with no .hs file at all, or preprocess-only: delegate everything */
    if (do_preprocess_only || !x_is_haskell ||
        (has_non_hs_source && !hs_input)) {
        delegate_to_cc(argc, argv);
    }

    /* Link mode: no source at all, just .o/.a files */
    if (!hs_input && ninputs > 0 && !has_non_hs_source) {
        delegate_to_cc(argc, argv);
    }

    /* Nothing to do */
    if (!hs_input) {
        fprintf(stderr,
            "qdhc: no Haskell input file.\n"
            "usage: qdhc [-c] [-o output] input.hs\n"
            "       qdhc [GCC flags] input.hs        # as a CC replacement\n"
            "  --run           JIT-compile and execute in-process\n"
            "  --dump-dir DIR  write cst.txt and ir.ll to DIR for debugging\n");
        return 1;
    }

    const char *inp = hs_input;

    /* Determine output path */
    char *outp_buf = NULL;
    const char *outp;
    if (flag_output) {
        outp = flag_output;
    } else if (do_compile_only) {
        /* -c with no -o: replace .hs with .o in the current directory */
        /* Use basename only — gcc puts foo.o in cwd, not next to source */
        const char *base = strrchr(inp, '/');
        base = base ? base+1 : inp;
        outp_buf = replace_ext(base, ".o");
        outp = outp_buf;
    } else if (do_assemble_only) {
        const char *base = strrchr(inp, '/');
        base = base ? base+1 : inp;
        outp_buf = replace_ext(base, ".s");
        outp = outp_buf;
    } else if (!do_run) {
        outp = "a.out";
    } else {
        outp = NULL; /* run mode: no output file */
    }

    FILE *f=fopen(inp,"rb"); if(!f){perror(inp);return 1;}
    fseek(f,0,SEEK_END); long fsz=ftell(f); rewind(f);
    char *src=malloc(fsz+1);
    if(fread(src,1,fsz,f) != (size_t)fsz){
        fprintf(stderr,"error: failed to read %s\n", inp); return 1;
    }
    src[fsz]=0; fclose(f);

    /* Pre-scan for GHC extensions that qdhc cannot compile */
    if(strstr(src,"_ccall_")||strstr(src,"_casm_")){
        fprintf(stderr,"error: unsupported GHC extension: _ccall_/_casm_ (foreign calls) in %s\n", inp);
        exit(1);
    }
    if(strstr(src,"I#")||strstr(src,"D#")||strstr(src,"F#")||strstr(src,"C#")){
        fprintf(stderr,"error: unsupported GHC extension: unboxed types (I#/D#/F#/C#) in %s\n", inp);
        exit(1);
    }

    TSParser *parser=ts_parser_new();
    const TSLanguage *lang=tree_sitter_haskell();
    ts_parser_set_language(parser,lang);
    TSTree *tree=ts_parser_parse_string(parser,NULL,src,(uint32_t)fsz);
    TSNode root=ts_tree_root_node(tree);

    if(ts_node_has_error(root)){
        fprintf(stderr,"error: parse errors in %s (unsupported syntax or GHC extensions)\n", inp);
        ts_tree_delete(tree); ts_parser_delete(parser); free(src);
        exit(1);
    }

    memset(&sym,0,sizeof sym);
    first_pass(root,src);

    /* When compiling only (-c), a missing main is fine — it will be linked
       in from another translation unit.  Only require main when producing
       an executable. */
    if (sym.nfuncs == 0) {
        fprintf(stderr,"error: no functions discovered in %s "
                "(imports, data, class, or instance only)\n", inp);
        ts_tree_delete(tree); ts_parser_delete(parser); free(src);
        exit(1);
    }

    if (!do_compile_only && !sym_func("main")) {
        fprintf(stderr,"error: no 'main' function in %s\n", inp);
        ts_tree_delete(tree); ts_parser_delete(parser); free(src);
        exit(1);
    }

    fprintf(stderr,"qdhc: %s — functions:", inp);
    for (int i = 0; i < sym.nfuncs; i++)
        fprintf(stderr," %s/%d", sym.funcs[i].name, sym.funcs[i].arity);
    fprintf(stderr,"\n");

    /* ── LLVM setup ─────────────────────────────────────────────────────── */
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();

    LLVMContextRef ctx  = LLVMContextCreate();
    LLVMModuleRef  mod  = LLVMModuleCreateWithNameInContext("haskell", ctx);
    LLVMBuilderRef bldr = LLVMCreateBuilderInContext(ctx);
    char *triple = LLVMGetDefaultTargetTriple();
    LLVMSetTarget(mod, triple);

    LLVMTargetRef  target_ref = NULL;
    char          *layout_err = NULL;
    LLVMTargetMachineRef tm   = NULL;

    if (!LLVMGetTargetFromTriple(triple, &target_ref, &layout_err)) {
        /* Use PIC relocation when -fPIC was passed or when building -shared,
           otherwise default.  PIC is also safer for cmake's object files. */
        LLVMRelocMode reloc = LLVMRelocPIC; /* always PIC — safest default */
        tm = LLVMCreateTargetMachine(
                target_ref, triple, "", "",
                LLVMCodeGenLevelDefault, reloc, LLVMCodeModelDefault);
        LLVMTargetDataRef dl = LLVMCreateTargetDataLayout(tm);
        LLVMSetModuleDataLayout(mod, dl);
        LLVMDisposeTargetData(dl);
        /* tm kept alive for object emission below */
    }
    LLVMDisposeMessage(layout_err);
    LLVMDisposeMessage(triple);

    CG cg = { .ctx=ctx, .mod=mod, .b=bldr,
              .i64=LLVMInt64TypeInContext(ctx),
              .i32=LLVMInt32TypeInContext(ctx),
              .src=src };

    predeclare(&cg);
    second_pass(&cg, root);

    /* Only emit the C-main wrapper when we're producing an executable or
       running via JIT.  In -c (compile-only) mode we emit the Haskell
       functions as plain symbols so the link step can combine them. */
    if (!do_compile_only && !do_assemble_only)
        emit_main(&cg);

    /* ── optional debug dumps ─────────────────────────────────────────── */
    if (dump_dir) {
        mkdir(dump_dir, 0755);
        char path[4096];

        snprintf(path, sizeof path, "%s/cst.txt", dump_dir);
        FILE *cst_f = fopen(path, "w");
        if (cst_f) { dump(root, src, 0, cst_f); fclose(cst_f); }
        else fprintf(stderr, "warning: could not write %s\n", path);

        snprintf(path, sizeof path, "%s/ir.ll", dump_dir);
        char *de = NULL;
        if (LLVMPrintModuleToFile(mod, path, &de))
            fprintf(stderr, "warning: could not write %s: %s\n", path, de?de:"");
        LLVMDisposeMessage(de);
    }

    /* ── verify ───────────────────────────────────────────────────────── */
    {
        char *err = NULL;
        if (LLVMVerifyModule(mod, LLVMPrintMessageAction, &err))
            fprintf(stderr, "qdhc: LLVM verify error: %s\n", err ? err : "");
        LLVMDisposeMessage(err);
    }

    /* ── JIT run (--run legacy mode) ──────────────────────────────────── */
    if (do_run) {
        LLVMLinkInMCJIT();
        LLVMExecutionEngineRef engine = NULL;
        char *jit_err = NULL;
        if (LLVMCreateMCJITCompilerForModule(&engine, mod, NULL, 0, &jit_err)) {
            fprintf(stderr, "qdhc: JIT error: %s\n", jit_err ? jit_err : "");
            LLVMDisposeMessage(jit_err);
            if (tm) LLVMDisposeTargetMachine(tm);
            LLVMDisposeBuilder(bldr); LLVMContextDispose(ctx);
            ts_tree_delete(tree); ts_parser_delete(parser); free(src);
            return 1;
        }
        LLVMValueRef main_fn = LLVMGetNamedFunction(mod, "main");
        int rc = LLVMRunFunctionAsMain(engine, main_fn, 0, NULL, NULL);
        LLVMDisposeExecutionEngine(engine);
        /* engine took ownership of mod */
        if (tm) LLVMDisposeTargetMachine(tm);
        LLVMContextDispose(ctx); LLVMDisposeBuilder(bldr);
        ts_tree_delete(tree); ts_parser_delete(parser); free(src);
        free(outp_buf);
        return rc;
    }

    /* ── -S: emit LLVM IR as the "assembly" output ────────────────────── */
    if (do_assemble_only) {
        char *err = NULL;
        if (LLVMPrintModuleToFile(mod, outp, &err))
            fprintf(stderr, "qdhc: write error: %s\n", err);
        LLVMDisposeMessage(err);
        goto done;
    }

    /* ── -c: emit native object file ──────────────────────────────────── */
    if (do_compile_only) {
        if (!tm) {
            fprintf(stderr, "qdhc: cannot create target machine for object emission\n");
            goto fail;
        }
        char *err = NULL;
        if (LLVMTargetMachineEmitToFile(tm, mod, (char*)outp,
                                        LLVMObjectFile, &err)) {
            fprintf(stderr, "qdhc: object emit error: %s\n", err ? err : "");
            LLVMDisposeMessage(err);
            goto fail;
        }
        goto done;
    }

    /* ── no -c: compile + link into an executable ─────────────────────── */
    /* Strategy: emit a temporary .o, then invoke cc to link it. */
    {
        char tmp_obj[] = "/tmp/qdhc_XXXXXX.o";
        int fd = mkstemps(tmp_obj, 2);
        if (fd < 0) { perror("mkstemps"); goto fail; }
        close(fd);

        if (!tm) {
            fprintf(stderr, "qdhc: cannot create target machine\n");
            unlink(tmp_obj); goto fail;
        }
        char *err = NULL;
        if (LLVMTargetMachineEmitToFile(tm, mod, tmp_obj, LLVMObjectFile, &err)) {
            fprintf(stderr, "qdhc: object emit error: %s\n", err ? err : "");
            LLVMDisposeMessage(err);
            unlink(tmp_obj); goto fail;
        }

        /* Link: cc -o <outp> <tmp_obj> */
        const char *cc = getenv("QDHC_CC");
        if (!cc || !*cc) cc = "cc";
        char *link_argv[8];
        link_argv[0] = (char*)cc;
        link_argv[1] = "-o"; link_argv[2] = (char*)outp;
        link_argv[3] = tmp_obj;
        link_argv[4] = NULL;
        pid_t pid = fork();
        if (pid == 0) { execvp(cc, link_argv); perror(cc); exit(1); }
        int status = 0;
        waitpid(pid, &status, 0);
        unlink(tmp_obj);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) goto fail;
    }

done:
    if (tm) LLVMDisposeTargetMachine(tm);
    LLVMDisposeBuilder(bldr); LLVMDisposeModule(mod); LLVMContextDispose(ctx);
    ts_tree_delete(tree); ts_parser_delete(parser); free(src);
    free(outp_buf);
    return 0;

fail:
    if (tm) LLVMDisposeTargetMachine(tm);
    LLVMDisposeBuilder(bldr); LLVMDisposeModule(mod); LLVMContextDispose(ctx);
    ts_tree_delete(tree); ts_parser_delete(parser); free(src);
    free(outp_buf);
    return 1;
}
