/*function toggleRelay(id) {
    fetch('/toggle?id=' + id)
      .then(() => updateStatus());
  }
  

  function updateStatus() {
    fetch('/api/status')
      .then(response => response.json())
      .then(data => {
        for (var i = 0; i < data.states.length; i++) {
          var button = document.getElementById('relay' + i);
          if (!button) continue;
  
          // Remove all existing classes and inline styles first
          button.classList.remove('green', 'red', 'active', 'has-dot');
          button.style.removeProperty('--dot-color');
  
          // Set button color
          if (data.states[i]) {
            button.classList.add('green', 'active');
          } else {
            button.classList.add('red', 'active');
          }
  
          // Set button label
          let label = data.labels && data.labels[i] ? data.labels[i] : button.innerText;
          button.innerText = label;
  
          // Decide dot color
          if (data.reset[i] && data.ips[i]) {
            button.classList.add('has-dot');
            if (data.states[i]) {
              // Relay is ON (green) ➔ yellow dot
              button.style.setProperty('--dot-color', 'yellow');
            } else {
              // Relay is OFF (red) ➔ grey dot
              button.style.setProperty('--dot-color', 'grey');
            }
          } else if (data.ips[i]) {
            button.classList.add('has-dot');
            if (data.states[i]) {
              // Relay is ON (green) ➔ blue dot
              button.style.setProperty('--dot-color', 'blue');
            } else {
              // Relay is OFF (red) ➔ grey dot
              button.style.setProperty('--dot-color', 'grey');
            }
          }
          // If no ping/reset, no dot needed (already cleared above)
        }
      });
  }
      
  
  function clearLog() {
    if (confirm('Are you sure you want to clear the log?')) {
      fetch('/clearlog')
        .then(response => {
          if (response.ok) {
            location.reload();
          } else {
            alert('Failed to clear log.');
          }
        });
    }
  }
  
  setInterval(updateStatus, 10000);
*/

function toggleRelay(id) {
  fetch('/toggle?id=' + id);
  // Optionally trigger delayed status update
  setTimeout(updateStatus, 1000); 
}

function updateStatus() {
  fetch('/api/status')
    .then(response => response.json())
    .then(data => {
      for (var i = 0; i < data.states.length; i++) {
        var button = document.getElementById('relay' + i);
        if (!button) continue;
        button.classList.remove('green', 'red', 'active', 'has-dot');
        button.style.removeProperty('--dot-color');

        if (data.states[i]) {
          button.classList.add('green', 'active');
        } else {
          button.classList.add('red', 'active');
        }

        let label = data.labels && data.labels[i] ? data.labels[i] : button.innerText;
        button.innerText = label;

        if (data.reset[i] && data.ips[i]) {
          button.classList.add('has-dot');
          button.style.setProperty('--dot-color', data.states[i] ? 'yellow' : 'grey');
        } else if (data.ips[i]) {
          button.classList.add('has-dot');
          button.style.setProperty('--dot-color', data.states[i] ? 'blue' : 'grey');
        }
      }
    });
}

  // Bit of a dogs dinner, but basically. This is called and redirects after 3 seconds.
  // At the same time, there is a reboot that is done 1 second after the button is clicked.
  function setPageTimeout(delay = 3000) {
    setTimeout(function() {
      window.location.href = "/";
    }, delay);
  }

// Will run as soon as the script is loaded by the browser
//setInterval(updateStatus, 2000);
updateStatus(); // fetches status immediately on page load


document.addEventListener('DOMContentLoaded', function () {
  document.getElementById('uploadInput').addEventListener('change', function () {
    if (this.files.length > 0) {
      // alert("Uploading file");
      document.getElementById('uploadForm').submit();
    }
  });
});


function triggerUpload() {

  if (confirm("If you upload a config.json that is not formatted correctly, the ESP32 will probably not work properly or connect to the wifi and you will have to reflash it. Want to continue? ")) {
    // User clicked OK
    console.log("Continuing...");
    // Continue with the rest of the function
  } else {
    // User clicked Cancel
    console.log("Aborted.");
    return; // Or break, depending on context
  }


  document.getElementById('uploadInput').click();
};


function togglePing(i) {
  const pingCheckbox = document.getElementById('ping' + i);
  const ipField = document.getElementById('ip' + i);
  const resetCheckbox = document.getElementById('reset' + i);

  if (pingCheckbox && ipField && resetCheckbox) {
    const enabled = pingCheckbox.checked;
    ipField.disabled = !enabled;
    resetCheckbox.disabled = !enabled;
  }
}

function toggleDeepSleep() {
  const deepSleepCheckbox = document.getElementById('globalSchedEnable');
  const wakeTimeInput = document.getElementById('globalOnTime');
  const sleepTimeInput = document.getElementById('globalOffTime');

  if (deepSleepCheckbox && wakeTimeInput && sleepTimeInput) {
    const disabled = !deepSleepCheckbox.checked;
    wakeTimeInput.disabled = disabled;
    sleepTimeInput.disabled = disabled;

    wakeTimeInput.style.color = disabled ? 'gray' : '';
    sleepTimeInput.style.color = disabled ? 'gray' : '';
  }
}

function triggerUpload() {
  const input = document.getElementById('uploadInput');
  if (input) {
    input.click();
  }
}

window.onload = function () {
  for (let i = 0; i < 6; i++) {
    togglePing(i);
  }
  toggleDeepSleep();

  const deepSleepCheckbox = document.getElementById('globalSchedEnable');
  if (deepSleepCheckbox) {
    deepSleepCheckbox.addEventListener('change', toggleDeepSleep);
  }
};


function clearLog() {
  if (confirm('Are you sure you want to clear the log?')) {
    fetch('/clearlog')
      .then(response => {
        if (response.ok) {
          location.reload();
        } else {
          alert('Failed to clear log.');
        }
      });
  }
}



