module M where
f n = if n == 0 then 1 else n * f (n - 1)
main = if f 10 == 3628800 then 0 else 1
