// Copyright 2021 Aaron Fontaine
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

var Pbf = require('pbf');
var SessionData = require('./session.js').SessionData;
var WiFiScanPayload = require('./wifi_scan.js').WiFiScanPayload;
var WiFiConfigPayload = require('./wifi_config.js').WiFiConfigPayload;

// Scan parameters
// There is a trade-off here between:
//   1. How many results are returned
//   2. How long it takes to scan all channels
//   3. Likelihood of causing the station requesting the scan to get disconnected
// See documentation for scan_start in:
// https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/provisioning/wifi_provisioning.html
const SCAN_IS_PASSIVE = false;      // True = passive sacn, False = active scan
const SCAN_CHANNEL_GROUPING = 3;    // Scan this many channels at a time before pausing to issue soft AP beacons
const SCAN_DWELL_TIME_MS = 150;     // Listen for this long per channel

const MAX_RESULTS_PER_REQUEST = 5;  // Number of results to be requested at a time
var numScanResults = 0;             // Set by the reponse to the Scan Status command
var scanIndex = 0;                  // Keeps track of how many results have been requested so far
var scanResults = [];

const RSSI_THRESHOLD = -90;         // Do not populate results at or below this threshold
const RESULTS_PER_PAGE = 5;         // Number of results to display at a time
var numScanResultsForDisplay = 0;   // Number of results after RSSI filter
var pageIndex = 0;                  // Current page of scan results being shown

// URIs when webpages are deployed to device, or being served by semihost.
const sessionUri = "/prov-session"
const scanUri = "/prov-scan"
const configUri = "/prov-config"
const customUri = "/prov-custom"

// *********************************************************************************************
// DEBUG: URIs when testing via localhost.
// const sessionUri = "http://192.168.4.1/prov-session"
// const scanUri = "http://192.168.4.1/prov-scan"
// const configUri = "http://192.168.4.1/prov-config"
// const customUri = "http://192.168.4.1/prov-custom"
// Note that allowing cross-domain requests of this nature requires modification to the ESP-IDF
// or the browser will interfere with the XMLHttpRequest on security grounds.
// Add the following line:
//   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
// in protocomm_httpd.c::common_post_handler() before the call to:
//   httpd_resp_send(req, (char *)outbuf, outlen);
// More info at: https://developer.mozilla.org/en-US/docs/Web/HTTP/CORS/Errors/CORSMissingAllowOrigin
// *********************************************************************************************

const provStates = {
    UNINITIALIZED:  0,      // Initial state
    INITIALIZING:   1,      // Requesting security access
    READY:          2,      // Idle
    SCANNING:       3,      // Host/server/ESP32 is performing network scan
    CONFIGURING:    4,      // In the process of sending credentials and applying them
    VERIFYING:      5,      // Waiting for success/fail confirmation on the attempt to join Wi-Fi
}

const WifiAuthModeStrings = [
    "None",
    "WEP",
    "WPA",
    "WPA2",
    "WPA/WPA2",
    "WPA2 Enterprise"
];

const WiFiConnectFailReasonStrings = [
    "Auth Failed",
    "Network Not Found",
];

const customCommands = {
    RESET_PROV:     0,
    SHUTDOWN_PROV:  1,
    GET_HOMEPAGE:   2,
};

const customCommandStrings = [
    "reset prov",
    "shutdown prov",
    "get homepage",
];

const customStatus = {
    SUCCESS:        0,
    BAD_JSON:       1,
    BAD_COMMAND:    2,
};

const customStatusStrings = [
    "ok",
    "bad json",
    "bad command",
];


window.onload = function() {
    document.getElementById("page-prev").addEventListener("click", showPrevResultsPage);
    document.getElementById("page-next").addEventListener("click", showNextResultsPage);
    document.getElementById("rescan").addEventListener("click", startScan);
    document.getElementById("apply").addEventListener("click", setConfig);
    document.getElementById("go-back").addEventListener("click", startOver);

    // Start in scanning/loading state.
    updateUIForScanning(true);

    // We are not initialized until we have been granted access via the session request.
    provState = provStates.UNINITIALIZED;

    // Request session with no security across the provisioning transport.
    requestSec0Session();
};

function startOver() {
    document.getElementById("ssid").value = "";
    document.getElementById("passphrase").value = "";
    startScan();
}

function requestSec0Session() {
    provState = provStates.INITIALIZING;

    // Requsting sec0 = No Security
    // (Security is already implicit due to passphrase needed to join soft AP)
    var payload = { sec_ver: 0 };
    payload.sec0 = { msg: 0 };
    payload.sec0.sc = { };

    // Convert this to protocol buffer byte format
    var pbf = new Pbf();
    SessionData.write(payload, pbf);
    var buffer = pbf.finish();

    // Issue sec0 protocomm request to the server
    sendProtocommRequest(buffer, "POST", sessionUri, handleSec0Response, true, 4000);
}

function handleSec0Response() {
    if (this.status == 200) {
        try {
            var pbf = new Pbf(new Uint8Array(this.response));
            var message = SessionData.read(pbf);
        
            console.log(message);
        
            if (message.sec_ver == 0 && message.sec0.sr.status==0) {
                console.log("Non-secure session granted.");
                provState = provStates.READY;

                // In the case of multiple auto-rise of provisioning page,
                // we want the separate webpages to work together to issue
                // only a single scan request. The first one to get a scan
                // status response will end up requesting the scan and the
                // other windows will wait for the results.
                getScanStatus();
            }
        } catch(e1) {
            alert(e1);
            // TODO: Indicate failure
        }
    } else {
        // TODO: Common error handler
        alert("Unexpected error: Failed to initialize provisioning.");
        updateUIForScanning(false);
    }
}

function startScan() {
    if (provState < provStates.READY) {
        alert("Provisioning endpoint not initialized. Try reloading page.");
        return;
    }

    provState = provStates.SCANNING;

    // Clear SSID table and give visual indication of scan in progress.
    deleteEntriesFromSsidTable();
    updateUIForScanning(true);
    scanResults = [];

    // Scan parameters:
    var payload = { msg: 0 };
    payload.cmd_scan_start = {
        blocking: true,
        passive: SCAN_IS_PASSIVE,
        group_channels: SCAN_CHANNEL_GROUPING,
        period_ms: SCAN_DWELL_TIME_MS
    };

    // Convert this to protocol buffer byte format
    var pbf = new Pbf();
    WiFiScanPayload.write(payload, pbf);
    var buffer = pbf.finish();

    // // Issue sec0 protocomm request to the server
    sendProtocommRequest(buffer, "POST", scanUri, handleScanStartResponse);
}

function handleScanStartResponse()
{
    if (this.status == 200) {
        try {
            var pbf = new Pbf(new Uint8Array(this.response));
            var message = WiFiScanPayload.read(pbf);
        
            console.log(message);
        
            if (message.msg == 1 && message.status == 0) {
                console.log("Scan completed successfully. Requesting scan results.");
                getScanStatus();
            }
        } catch(e1) {
            alert(e1);
            // TODO: Indicate failure
        }
    } else {
        // TODO: Indicate failure
    }
}

function getScanStatus() {
    // Scan parameters:
    var payload = { msg: 2 };
    payload.cmd_scan_status = {  };

    // Convert this to protocol buffer byte format
    var pbf = new Pbf();
    WiFiScanPayload.write(payload, pbf);
    var buffer = pbf.finish();

    // // Issue sec0 protocomm request to the server
    sendProtocommRequest(buffer, "POST", scanUri, handleGetScanStatusResponse);
}

function handleGetScanStatusResponse()
{
    if (this.status == 200) {
        try {
            var pbf = new Pbf(new Uint8Array(this.response));
            var message = WiFiScanPayload.read(pbf);
        
            console.log(message);
        
            if (message.msg == 3 && message.status == 0) {
                handleScanStatus(message.resp_scan_status);
            }
        } catch(e1) {
            alert(e1);
            // TODO: Indicate failure
        }
    } else {
        // TODO: Indicate failure
    }
}

function handleScanStatus(scanStatus) {
    if (scanStatus.scan_finished) {
        if (scanStatus.result_count > 0) {
            // A scan has already been performed and we need
            // lonly collect the results.
            numScanResults = scanStatus.result_count;
            numScanResultsForDisplay = numScanResults;
            console.log("Found %d networks.", numScanResults);
            scanIndex = 0;
            updateUIForScanning(false);
            getScanResults();
        } else {
            // No scan has been initiated yet. We must issue
            // scan request to collect initial results.
            console.log("No scan requested yet. Requesting initial scan...");
            startScan();
        }
    } else {
        // A scan has already been requested, either by this
        // browser window or another one. We will wait for it
        // to finish and collect its results.
        console.log("Scan in progress. Checking again in 1 second...");
        setTimeout(getScanStatus, 1000);
    }
}

function getScanResults() {
    if (scanIndex < numScanResults) {
        // Requst up to MAX_RESULTS_PER_REQUEST results.
        var numResultsToRequest = Math.min(numScanResults - scanIndex, MAX_RESULTS_PER_REQUEST);
        var payload = { msg: 4 };
        payload.cmd_scan_result = {
            start_index: scanIndex,
            count: numResultsToRequest,
        };
        scanIndex += numResultsToRequest;

        // Convert this to protocol buffer byte format
        var pbf = new Pbf();
        WiFiScanPayload.write(payload, pbf);
        var buffer = pbf.finish();

        // Issue request
        sendProtocommRequest(buffer, "POST", scanUri, handleScanResultsResponse);
    } else {
        // No more results to retrieve.
        console.log("Scan operation complete");
        provState = provStates.READY;

        // Show first page of results
        console.log("Total results for display = %d", numScanResultsForDisplay);
        pageIndex = 0;
        showSelectedResults();
    }
}

function handleScanResultsResponse()
{
    if (this.status == 200) {
        try {
            var pbf = new Pbf(new Uint8Array(this.response));
            var message = WiFiScanPayload.read(pbf);
        
            console.log(message);
        
            if (message.msg == 5 && message.status == 0) {
                console.log("Retrieved " + message.resp_scan_result.entries.length + " results.");

                // Add these entries to the SSID table
                for (let i = 0; i < message.resp_scan_result.entries.length; i++) {
                    var scanResult = message.resp_scan_result.entries[i];

                    // Completely throw any results not meeting the threshold
                    //   and decrement the results count when we do so.
                    if (scanResult.rssi > RSSI_THRESHOLD) {
                        scanResults.push(scanResult);
                    } else {
                        console.log("Discarded entry with RSSI %d", scanResult.rssi);
                        numScanResultsForDisplay--;
                    }
                }

                // Request the next batch of results
                getScanResults();
            }
        } catch(e1) {
            alert(e1);
            // TODO: Indicate failure
        }
    } else {
        // TODO: Indicate failure
    }
}

function setConfig() {
    if (provState < provStates.READY) {
        alert("Provisioning endpoint not initialized. Try reloading page.");
        return;
    }
    
    if (!verifyConfig())
        return;

    provState = provStates.CONFIGURING;
    
    ssidTxt = document.getElementById("ssid").value;
    passTxt = document.getElementById("passphrase").value;

    updateUIForConnecting(true, false, ssidTxt);

    // Send a Set Config command
    var encoder = new TextEncoder();
    var payload = { msg: 2 };
    payload.cmd_set_config = {
        ssid: encoder.encode(ssidTxt),
        passphrase: encoder.encode(passTxt),
    };

    // Convert this to protocol buffer byte format
    var pbf = new Pbf();
    WiFiConfigPayload.write(payload, pbf);
    var buffer = pbf.finish();

    // // Issue sec0 protocomm request to the server
    sendProtocommRequest(buffer, "POST", configUri, handleSetConfigResponse);
}

function handleSetConfigResponse() {
    if (this.status == 200) {
        try {
            var pbf = new Pbf(new Uint8Array(this.response));
            var message = WiFiConfigPayload.read(pbf);
        
            console.log(message);
        
            if (message.msg == 3 && message.resp_set_config.status == 0) {
                console.log("Configuration sent successfully.");
                applyConfig();
            }
        } catch(e1) {
            alert(e1);
            // TODO: Indicate failure
        }
    } else {
        // TODO: Indicate failure
        console.log("Failed to send configuration");
    }
}

function applyConfig() {
    // Send a Set Config command
    var payload = { msg: 4 };
    payload.cmd_apply_config = { };

    // Convert this to protocol buffer byte format
    var pbf = new Pbf();
    WiFiConfigPayload.write(payload, pbf);
    var buffer = pbf.finish();

    // // Issue sec0 protocomm request to the server
    sendProtocommRequest(buffer, "POST", configUri, handleApplyConfigResponse);
}

function handleApplyConfigResponse() {
    if (this.status == 200) {
        try {
            var pbf = new Pbf(new Uint8Array(this.response));
            var message = WiFiConfigPayload.read(pbf);
        
            console.log(message);
        
            if (message.msg == 5 && message.resp_apply_config.status == 0) {
                console.log("Configuration applied successfully.");
                setTimeout(getConfigStatus, 5000);
            }
        } catch(e1) {
            alert(e1);
            // TODO: Indicate failure
        }
    } else {
        // TODO: Indicate failure
        console.log("Failed to apply configuration");
    }
}

function getConfigStatus() {
    // Send a Set Config command
    var payload = { msg: 0 };
    payload.cmd_get_status = { };

    // Convert this to protocol buffer byte format
    var pbf = new Pbf();
    WiFiConfigPayload.write(payload, pbf);
    var buffer = pbf.finish();

    // // Issue sec0 protocomm request to the server
    sendProtocommRequest(buffer, "POST", configUri, handleGetConfigStatusResponse);
}

function handleGetConfigStatusResponse() {
    if (this.status == 200) {
        try {
            var pbf = new Pbf(new Uint8Array(this.response));
            var message = WiFiConfigPayload.read(pbf);
        
            console.log(message);
        
            if (message.msg == 1) {
                if (message.resp_get_status.status == 0) {
                    if (message.resp_get_status.sta_state == 0) {
                        updateUIForConnecting(false, true, "Success");
                        sendCustomCommand(customCommands.SHUTDOWN_PROV, handleShutdownProvRepsonse);
                    } else if (message.resp_get_status.sta_state == 1) {
                        // Still in connecting state.  Check again in 1 second.
                        setTimeout(getConfigStatus, 1000);
                    } else {
                        // Station state 2 = Disconnected, Unclear why or if we should ever get this.
                        // Station state 3 = Connection Failed
                        updateUIForConnecting(false, false, WiFiConnectFailReasonStrings[message.resp_get_status.fail_reason]);
                        sendCustomCommand(customCommands.RESET_PROV, handleResetProvResponse);
                    }
                } else {
                    console.log("Configuration attempt not complete. Status = %d", message.resp_get_status.status);
                    setTimeout(getConfigStatus, 1000);
                }
            }
        } catch(e1) {
            alert(e1);
            // TODO: Indicate failure
        }
    } else {
        // TODO: Indicate failure
        console.log("Failed to get config status.");
    }
}

function getHomepageUrl() {
    sendCustomCommand(customCommands.GET_HOMEPAGE, handleGetHomepageResponse);
}

function sendCustomCommand(cmdNumber, responseHandler) {
    cmd = { command: customCommandStrings[cmdNumber] };
    buffer = JSON.stringify(cmd);
    sendProtocommRequest(buffer, "POST", customUri, responseHandler, false);
}

function handleResetProvResponse() {
    if (this.status == 200) {
        var resp = JSON.parse(this.responseText);
        if (resp.status === customStatusStrings[customStatus.SUCCESS]) {
            console.log("Provisioning manager reset");
        } else {
            console.log("Reset prov command failed with status: " + resp.status);
        }
    } else {
        // TODO: Indicate failure
        console.log("Reset prov command failed.");
    }
}

function handleShutdownProvRepsonse() {
    if (this.status == 200) {
        var resp = JSON.parse(this.responseText);
        if (resp.status === customStatusStrings[customStatus.SUCCESS]) {
            console.log("Provisioning manager shut down");
            getHomepageUrl();
        } else {
            console.log("Shutdown prov command failed with status: " + resp.status);
        }
    } else {
        // TODO: Indicate failure
        console.log("Shutdown prov command failed.");
    }
}

function handleGetHomepageResponse() {
    if (this.status == 200) {
        var resp = JSON.parse(this.responseText);
        if (resp.status === customStatusStrings[customStatus.SUCCESS]) {
            console.log("Got homepage URL: " + resp.uri);

            // Show the user the URL for their device homepage.
            showRedirectAddress(resp.uri);
            
            // This won't actually exit the CNA (Captive Network Assistant) or the
            //   CPMB (Captive-Portal Mini Browser) on all devices.
            // As such, this auto-redirect may not be the best flow.
            window.location = resp.uri;
        } else {
            console.log("Failed to get homepage URL: " + resp.status);
        }
    } else {
        // TODO: Indicate failure
        console.log("Failed to get homepage URL.");
    }
}

function sendProtocommRequest(buffer, method, uri, onLoadCallback, isBinary=true, timeout=13000)
{
    try {
        // Note: Most examples access the XMLHttpRequest object in the callbacks by
        // assigning anonymous functions directly to onload, ontimeout, etc. This
        // allows xhr to be in the same scope.
        // However, with callbacks as they are done here, the XMLHttpRequest object
        // is still accessible as the "this" variable. I mention this because, as 
        // someone who is not seasoned in JavaScript, it was not intuitive to me.
        var xhr = new XMLHttpRequest();
        xhr.open(method, uri);
        xhr.setRequestHeader('Accept', 'text/plain');
        if (isBinary) {
            xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
            xhr.responseType = 'arraybuffer';
        } else {
            xhr.setRequestHeader('Content-Type', 'text/plain');
        }
        xhr.timeout = timeout;
        xhr.ontimeout = onProvRequestTimeout;
        xhr.onload = onLoadCallback;
        xhr.send(buffer);
    } catch (e1) {
        alert(e1);
    }
}

function onProvRequestTimeout() {
    alert("Unexpected error: Failed to communicate with provisioining endpoint.");
    provState = provStates.UNINITIALIZED;
    updateUIForScanning(false);
}

function verifyConfig() {
    ssidTxt = document.getElementById("ssid").value;
    passTxt = document.getElementById("passphrase").value;

    if (ssidTxt.length == 0) {
        alert("Error: No SSID specified.");
        return false;
    }
    if (ssidTxt.length > 31) {
        alert("Error: SSID too long.");
        return false;
    }
    if (passTxt.length > 0 && passTxt.length < 8) {
        alert("Error: Passphrase must be at least 8 characters or left blank for no security.");
        return false;
    }
    if (passTxt.length > 63) {
        alert("Error: Passphrase too long.");
        return false;
    }
    return true;
}

function deleteEntriesFromSsidTable() {
    var ssidTable = document.getElementById('ssid-table').getElementsByTagName('tbody')[0];
    while (ssidTable.rows.length >= 1) {
        ssidTable.deleteRow(-1);    // Delete last row from table
    }
}

function addEntryToSsidTable(scanResult) {
    var ssidStr = new TextDecoder("utf-8").decode(scanResult.ssid);

    var btn = document.createElement("INPUT");
    btn.setAttribute('type', 'button');
    btn.setAttribute('class', 'btn btn-primary');
    btn.setAttribute('value', ssidStr);
    btn.addEventListener("click", onFocus);

    var ssidTable = document.getElementById('ssid-table').getElementsByTagName('tbody')[0];
    var newRow = ssidTable.insertRow(-1);

    if (newRow.rowIndex % 2 == 1) {
        newRow.setAttribute('class', 'active');
    }

    newRow.insertCell(-1).appendChild(btn);
    newRow.insertCell(-1).innerHTML = scanResult.rssi;
    newRow.insertCell(-1).innerHTML = WifiAuthModeStrings[scanResult.auth];
}

function updateUIForMode(isScanning) {
    
    var scanningDiv = document.getElementById("scanning-div");
    var configDiv = document.getElementById("connecting-div");
    var redirectDiv = document.getElementById("redirect-div");
    if (isScanning) {
        scanningDiv.style.display = "block";
        configDiv.style.display = "none";
    } else {
        scanningDiv.style.display = "none";
        configDiv.style.display = "block";
    }
    redirectDiv.hidden = true;
}

function updateUIForScanning(isScanning) {
    updateUIForMode(true);

    var progressDiv = document.getElementById("scan-ind");
    var pageSelectDiv = document.getElementById("page-select");
    var ssidFormInputs = document.getElementById("ssid-form").getElementsByTagName("input");
    if (isScanning) {
        progressDiv.style.display = "block";
        pageSelectDiv.style.display = "none";
        for (const inpt of ssidFormInputs) {
            inpt.disabled = true;
        }
    } else {
        progressDiv.style.display = "none";
        pageSelectDiv.style.display = "block";
        for (const inpt of ssidFormInputs) {
            inpt.disabled = false;
        }
    }
}

function updateUIForConnecting(isInProgress, isSuccess, msgText) {
    updateUIForMode(false);

    var ssidTxt = document.getElementById("connect-msg");
    var ssidLdr = document.getElementById("connect-ldr");
    var resultTxt = document.getElementById("result-msg");
    var goBackBtn = document.getElementById("go-back");

    goBackBtn.style.display = "none";

    if (isInProgress) {
        if (msgText.length > 0) {
            ssidTxt.innerHTML = "Connecting to " + msgText;
        }
        ssidLdr.style.display = "block";
        resultTxt.style.display = "none";
    } else {
        ssidLdr.style.display = "none";
        resultTxt.style.display = "block";
        resultTxt.innerHTML = msgText;
        if (!isSuccess) {
            goBackBtn.style.display = "block";
        }
    }
}

function showSelectedResults() {
    deleteEntriesFromSsidTable();
    var scanResultsIndex = RESULTS_PER_PAGE * pageIndex;
    var numResultsToShow = Math.min(RESULTS_PER_PAGE, numScanResultsForDisplay - scanResultsIndex);
    var scanResultsEnd = scanResultsIndex + numResultsToShow;
    for (let i = scanResultsIndex; i < scanResultsEnd; i++) {
        addEntryToSsidTable(scanResults[i]);
    }

    var prevBtn = document.getElementById("page-prev");
    if (scanResultsIndex == 0) {
        prevBtn.disabled = true;
    } else {
        prevBtn.disabled = false;
    }


    var nextBtn = document.getElementById("page-next");
    if (scanResultsEnd >= numScanResultsForDisplay) {
        nextBtn.disabled = true;
    } else {
        nextBtn.disabled = false;
    }
}

function showNextResultsPage() {
    pageIndex++;
    showSelectedResults();
}

function showPrevResultsPage() {
    pageIndex--;
    showSelectedResults();
}

function showRedirectAddress(url)
{
    var redirectLink = document.getElementById("redirect-a");
    var redirectDiv = document.getElementById("redirect-div");

    redirectLink.href = url;
    redirectLink.innerHTML = url;
    redirectDiv.hidden = false;
}

function onFocus() {
    document.getElementById('ssid').value = this.value;
    document.getElementById('passphrase').focus();
}
