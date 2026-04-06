module M where
s n = if n > 0 then 1 else if n < 0 then -1 else 0
main = if s 99 == 1 then 0 else 1
