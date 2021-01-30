# Override of wifi\_provisioning component provided in ESP-IDF framework

 The only change made to the wifi\_provisioning component is the addition of the wifi\_prov\_mgr\_reset\_to\_ready\_state() function. This was added to allow additional attempts to assign Wi-Fi credentials if the first attempt fails. The original design does not allow this and is documented as follows in the Espressif API reference.

> \[I\]f device was not able to connect using the provided Wi-Fi credentials, due to incorrect SSID / passphrase, the service will keep running, and get\_status will keep responding with disconnected status and reason for disconnection. Any further attempts to provide another set of Wi-Fi credentials, will be rejected. These credentials will be preserved, unless the provisioning service is force started, or NVS erased.

Without the addition of the wifi\_prov\_mgr\_reset\_to\_ready\_state() function, there is no way to return to the WIFI\_PROV\_STATE\_STARTED state and allow for additional scan and set config operations. This function also clears the Wi-Fi station credentials from NVS and sets the Wi-Fi config back to an empty RAM configuration as though the wifi\_provisioning component had just been initialized.
