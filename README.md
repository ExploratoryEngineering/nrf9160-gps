# nrf9160-gps
An example of how to use GPS on the nRF9160.

This example collects GPS position data at a rate of one sample per second and sends them over LTE-M.

The nRF9160 shares a single radio for GPS and LTE, and the current modem firmware (0.7.0-29.alpha) only allows one to be active at a time.  We therefore have to switch back and forth between these modes in order to collect GPS data and send it.  This switching introduces a delay which we minimize by buffering position data and sending it in batches.  A future modem firmware will allow both modes to be active simultaneously, which will enable streaming of location data.

## Building

TODO
