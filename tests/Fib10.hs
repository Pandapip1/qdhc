module M where
fib n = if n <= 1 then n else fib (n-1) + fib (n-2)
main = if fib 10 == 55 then 0 else 1
