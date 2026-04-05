module Main where

double :: Int -> Int
double x = x + x

factorial :: Int -> Int
factorial n = if n == 0 then 1 else n * factorial (n - 1)

add :: Int -> Int -> Int
add x y = x + y

main :: IO ()
main = do
    let result = double 21
    let fact5 = factorial 5
    let sum = add 10 32
    result
