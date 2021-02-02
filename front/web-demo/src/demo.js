
// Uncomment this line for deployment
const webApiUri = "/web-api"

// Uncomment this line when testing from development computer as localhost
// Be sure to also uncomment:
//   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
// In rest_web_api_handler() in webprov_example_main.c
// const webApiUri = "http://192.168.4.1/web-api"


var updateTimer;

window.onload = function() {
    updatePage();
    document.getElementById("clear-wifi").addEventListener("click", clearWifiSettings);
};

function updatePage() {
    // Update operations are now handled sequentially, not simultaneously.
    // The timeout for the next updatePage() is not started until the
    // current command sequence completes.
    updateUptime();
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
            updateButtonState();
        } else {
            console.log("Failed to get system uptime: " + resp.status);
            handleFailure();
        }
    } else {
        console.log("Failed to get system uptime." + this.status);
        handleFailure();
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
            setTimeout(updatePage, 300);
        } else {
            console.log("Failed to get button state: " + resp.status);
            handleFailure();
        }
    } else {
        console.log("Failed to get button state." + this.status);
        handleFailure();
    }
};

function clearWifiSettings() {
    cmd = { command: "clear wifi settings" };
    buffer = JSON.stringify(cmd);
    sendWebAPIRequest(buffer, handleClearWifiSettingsResponse, handleClearWifiSettingsTimeout, 8000);
}

function handleClearWifiSettingsResponse() {
    if (this.status == 200) {
        var resp = JSON.parse(this.responseText);
        if (resp.status === "ok") {
            clearInterval(updateTimer);
            alert("Command successful. Connection to the device will be lost.");
        } else {
            alert("Failed to clear wifi settings: " + resp.status);
        }
    } else {
        alert("Failed to clear wifi settings." + this.status);
    }
}

function handleClearWifiSettingsTimeout() {
    alert("Failed to clear wifi settings: command timed out");
}

function sendWebAPIRequest(buffer, onLoadCallback, timeoutFunc=onWebAPIRequestTimeout, timeout=5000) {
    try {
        var xhr = new XMLHttpRequest();
        xhr.open("POST", webApiUri);
        xhr.setRequestHeader('Content-Type', 'text/plain');
        xhr.setRequestHeader('Accept', 'text/plain');
        xhr.timeout = timeout;
        xhr.ontimeout = timeoutFunc;
        xhr.onload = onLoadCallback;
        xhr.send(buffer);
    } catch (e1) {
        alert(e1);
    }
}

function onWebAPIRequestTimeout() {
    console.log("HTTP request timeout.");
    handleFailure();
}

function handleFailure() {
    // Fail silently and keep trying.
    setTimeout(updatePage, 1000);
}