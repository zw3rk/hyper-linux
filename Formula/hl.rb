# Homebrew formula for hyper-linux (hl)
#
# Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Install: brew install zw3rk/hyper-linux/hl
#   (auto-taps github.com/zw3rk/homebrew-hyper-linux)
#
# This formula downloads a pre-built binary from GitHub Releases.
# Codesigning with Hypervisor.framework entitlement happens in
# post_install so it persists across `brew upgrade`.

class Hl < Formula
  desc "Run aarch64/x86_64 Linux ELF binaries on macOS Apple Silicon"
  homepage "https://hyper-linux.app"
  url "https://github.com/zw3rk/hyper-linux/releases/download/v0.1.1/hl-unknown.zip"
  sha256 "d671f89f2249437b065607561f86896768a213bd27b7bb1053f8e9c9cf97eef3"
  version "0.1.1"
  license "Apache-2.0"

  depends_on :macos
  depends_on arch: :arm64

  def install
    bin.install "hl"
    man1.install "hl.1"
    # Store entitlements for post_install codesigning and manual re-signing
    (share/"hl").install "entitlements.plist"
  end

  def post_install
    # Ad-hoc codesign with Hypervisor.framework entitlement.
    # Must run on the end-user machine (ad-hoc signatures are local).
    system "codesign", "--entitlements", "#{share}/hl/entitlements.plist",
           "-f", "-s", "-", "#{bin}/hl"
  end

  def caveats
    <<~EOS
      hyper-linux requires Apple Silicon (M1/M2/M3/M4 or later).
      macOS 15+ is recommended for the full 1TB guest address space.

      The binary has been codesigned with an ad-hoc signature for
      Hypervisor.framework access.  If you see "hv_vm_create failed",
      re-sign manually:
        codesign --entitlements #{share}/hl/entitlements.plist -f -s - #{bin}/hl
    EOS
  end

  test do
    assert_match "hl", shell_output("#{bin}/hl --version")
  end
end
