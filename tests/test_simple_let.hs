module Main where

double :: Int -> Int
double x = x + x

main :: IO ()
main = let r = double 7 in r
