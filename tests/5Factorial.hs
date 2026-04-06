module M where
f n = if n == 0 then 1 else n * f (n - 1)
main = if f 5 == 120 then 0 else 1
