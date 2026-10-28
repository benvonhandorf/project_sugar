[← docs](README.md) · [Project README](../README.md)

# Bring-up order

`app_main()` is a list of "set this up", not a sequence of "wait for that". **Nothing
below waits for the network**: every service that needs connectivity subscribes to
`NET_EVENT` and starts itself when a link appears.

That is why the order is short and the reasons are few:

1. **`diag_init()` first.** Output before anything that might fail, so failures are
   visible — on the serial port now, and on the web console and MQTT once those exist.
2. **NVS, netif, the default event loop.** `NET_EVENT` is posted to the default loop, so
   it has to exist before anything subscribes.
3. **Configuration.** Everything after this is configured from it. A missing or invalid
   file is logged and *not* fatal: schema defaults leave the device reachable, which is
   the state from which it can be fixed.
4. **The shell, before the network.** A device that never joins anything must still be
   reachable over the serial port.
5. **Services** — mDNS, NTP, MQTT, HTTP. Registration order does not matter and none of
   them blocks; they are all waiting for the same event.
6. **WiFi last.** It is what makes `NET_EVENT_LINK_UP` happen, so everything that reacts
   to it is already listening by the time it can fire.
7. **Confirm the OTA image, at the very end.** See [ota.md](ota.md) — doing this earlier
   would confirm images that never actually worked.

## Adding a service

If it needs the network, subscribe to `NET_EVENT` rather than adding a wait. If it does
not, start it wherever it belongs in the list above. Either way it should not block:
`app_main()` returning promptly is what lets the console come up on a device whose
network never does.
