module Main where

-- Basic arithmetic
double :: Int -> Int
double x = x + x

-- Multi-argument
add :: Int -> Int -> Int
add x y = x + y

mul3 :: Int -> Int -> Int -> Int
mul3 x y z = x * y * z

-- Recursive with if/then/else
factorial :: Int -> Int
factorial n = if n == 0 then 1 else n * factorial (n - 1)

-- Fibonacci (double recursion)
fib :: Int -> Int
fib n = if n <= 1 then n else fib (n - 1) + fib (n - 2)

-- Guard-style via nested ifs
sign :: Int -> Int
sign n = if n > 0 then 1 else if n < 0 then -1 else 0

-- let-in binding
circleArea :: Int -> Int
circleArea r =
    let r2 = r * r
        pi3 = 3
    in pi3 * r2

main :: IO ()
main =
    let d  = double 21
        f5 = factorial 5
        f10 = factorial 10
        fi7 = fib 7
        s1  = sign 42
        s2  = sign (-7)
        s3  = sign 0
        ca  = circleArea 5
        a   = add 100 23
        m   = mul3 2 3 7
    in d
