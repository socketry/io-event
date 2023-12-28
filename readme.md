# ![Event](logo.svg)

Provides low level cross-platform primitives for constructing event loops, with support for `select`, `kqueue`, `epoll` and `io_uring`.

[![Development Status](https://github.com/socketry/io-event/workflows/Test/badge.svg)](https://github.com/socketry/io-event/actions?workflow=Test)

## Motivation

The initial proof-of-concept [Async](https://github.com/socketry/async) was built on [NIO4r](https://github.com/socketry/nio4r). It was perfectly acceptable and well tested in production, however being built on `libev` was a little bit limiting. I wanted to directly built my fiber scheduler into the fabric of the event loop, which is what this gem exposes - it is specifically implemented to support building event loops beneath the fiber scheduler interface, providing an efficient C implementation of all the core operations.

## Usage

Please see the [project documentation](https://socketry.github.io/io-event/).

## Contributing

We welcome contributions to this project.

1.  Fork it.
2.  Create your feature branch (`git checkout -b my-new-feature`).
3.  Commit your changes (`git commit -am 'Add some feature'`).
4.  Push to the branch (`git push origin my-new-feature`).
5.  Create new Pull Request.

### Developer Certificate of Origin

This project uses the [Developer Certificate of Origin](https://developercertificate.org/). All contributors to this project must agree to this document to have their contributions accepted.

### Contributor Covenant

This project is governed by the [Contributor Covenant](https://www.contributor-covenant.org/). All contributors and participants agree to abide by its terms.
