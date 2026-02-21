-- | Simple Hello World to verify hl can execute GHC-produced aarch64-linux ELF binaries.
--
-- Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
-- SPDX-License-Identifier: Apache-2.0
module Main where

main :: IO ()
main = putStrLn "Hello, Hyper Linux!"
