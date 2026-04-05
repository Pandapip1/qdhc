module Main where

factorial :: Int -> Int
factorial n = if n == 0 then 1 else n * factorial (n - 1)

fib :: Int -> Int
fib n = if n <= 1 then n else fib (n - 1) + fib (n - 2)

sign :: Int -> Int
sign n = if n > 0 then 1 else if n < 0 then -1 else 0

mul3 :: Int -> Int -> Int -> Int
mul3 x y z = x * y * z

main :: IO ()
main =
    let f5  = factorial 5
        f10 = factorial 10
        fi7 = fib 7
        s1  = sign 99
        s2  = sign (-3)
        s3  = sign 0
        m   = mul3 2 3 7
    in f5
