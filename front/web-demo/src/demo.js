
// Uncomment this line for deployment
const webApiUri = "/web-api"

// Uncomment this line when testing from development computer as localhost
// Be sure to also uncomment:
//   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
// In rest_web_api_handler() in webprov_example_main.c
// const webApiUri = "http://192.168.4.1/web-api"


var updateTimer;

window.onload = function() {
    // TODO: Don't use setInterval. Wait for previous command to succeed or fail
    // before issuing the next one.
    updateTimer = setInterval(updatePage, 300);
    document.getElementById("clear-wifi").addEventListener("click", clearWifiSettings);
};

function updatePage() {
    // TODO: Use synchronization constructs so that these XHR requests don't
    // keep executing in the background if one of them is timing out.
    updateUptime();
    updateButtonState();
}

function updateUptime() {
    cmd = { command: "get system uptime" };
    buffer = JSON.stringify(cmd);
    sendWebAPIRequest(buffer, handleGetSystemUptimeResponse);
}

function handleGetSystemUptimeResponse() {
    if (this.status == 200) {
    var resp = JSON.parse(this.responseText);
        if (resp.status === "ok") {
            var uptime_div = document.getElementById("dev-uptime");
            uptime_div.innerHTML = resp.uptime;
        } else {
            console.log("Failed to get system uptime: " + resp.status);
        }
    } else {
        // TODO: Indicate failure
        console.log("Failed to get system uptime.");
    }
};

function updateButtonState() {
    cmd = { command: "get button state" };
    buffer = JSON.stringify(cmd);
    sendWebAPIRequest(buffer, handleGetButtonStateResponse);
}

function handleGetButtonStateResponse() {
    if (this.status == 200) {
    var resp = JSON.parse(this.responseText);
        if (resp.status === "ok") {
            var uptime_div = document.getElementById("button-state");
            uptime_div.innerHTML = resp.button;
        } else {
            console.log("Failed to get button state: " + resp.status);
        }
    } else {
        // TODO: Indicate failure
        console.log("Failed to get button state.");
    }
};

function clearWifiSettings() {
    cmd = { command: "clear wifi settings" };
    buffer = JSON.stringify(cmd);
    sendWebAPIRequest(buffer, handleClearWifiSettingsResponse);
}

function handleClearWifiSettingsResponse() {
    if (this.status == 200) {
        var resp = JSON.parse(this.responseText);
            if (resp.status === "ok") {
                clearInterval(updateTimer);
                alert("Command successful. Connection to the device will be lost.");
            } else {
                console.log("Failed to clear wifi settings: " + resp.status);
            }
        } else {
            // TODO: Indicate failure
            console.log("Failed to clear wifi settings.");
        }
    }

function sendWebAPIRequest(buffer, onLoadCallback, timeout=2000) {
    try {
        var xhr = new XMLHttpRequest();
        xhr.open("POST", webApiUri);
        xhr.setRequestHeader('Content-Type', 'text/plain');
        xhr.setRequestHeader('Accept', 'text/plain');
        xhr.timeout = timeout;
        xhr.ontimeout = onWebAPIRequestTimeout;
        xhr.onload = onLoadCallback;
        xhr.send(buffer);
    } catch (e1) {
        alert(e1);
    }
}

function onWebAPIRequestTimeout() {
    // TODO: Fail silently and keep trying.
    clearInterval(updateTimer);
    alert("Unexpected error: Failed to communicate with web api.");
}
