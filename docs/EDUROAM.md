# eduroam on Windows with an Intel AX201

This field report gives a working reference for the planned 802.1X station support. It covers one eduroam deployment. Other deployments may use a different EAP method or trust configuration.

## Tested configuration

- Windows 11, Intel Wi-Fi 6 AX201, nekodrop 2.1.4 Preview in On-Demand mode. Windows owned the card throughout the test.
- The user selected a saved eduroam profile in Windows. The interface reported WPA2-Enterprise (FT) on 5 GHz, channel 157.
- The profile specified outer PEAP (EAP type 25) and inner EAP-MSCHAPv2 (type 26), with server certificate validation enabled and trusted roots configured.

## Successful connection

Times are relative to the start of 802.1X authentication. Event IDs come from the Windows WLAN-AutoConfig Operational log.

| Elapsed | Event ID | Observation |
| ---: | ---: | --- |
| −0.015 s | 11001 | The station associated with the access point. |
| 0.000 s | 12011 | Windows started 802.1X authentication with EAP type 25. |
| 27.712 s | 12012 | 802.1X authentication succeeded. |
| 27.740 s | 8001 | Windows reported the WLAN connection established. |

An HTTP request bound to the Wi-Fi interface's IPv4 address returned HTTP 200 and the expected Microsoft Connect Test body. Binding the request matters on a machine with a second internet connection.

For a future Concurrent-mode implementation, the comparable milestones are association, successful enterprise authentication with the site's trust settings, and traffic through the nekodrop interface. This run establishes the Windows baseline only. The elapsed time is one observation, not a timeout recommendation.

To compare results from another deployment, report the EAP methods, whether server validation was enabled, relative timings and the interface-bound connectivity result. These fields can be shared without exporting the wireless profile, certificate identities, credentials or a packet capture.
