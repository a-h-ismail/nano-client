# nano-client

## Purpose

This repository hosts a modified version of the `nano` editor that implements a basic remote file editing protocol, for use with my other repo (realtime-text).

## Build

Refer to the instructions by the original developers in `README.hacking` (but clone this repo instead of the upstream).

## Usage

The modified nano is launched in client mode as follows:

```
./nano --client IPv4_addr filename
```

It will attempt to connect to the server at the specified IP and port 12000.
