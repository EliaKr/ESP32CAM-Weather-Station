const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-CAM Web Server</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            max-width: 800px;
            margin: 50px auto;
            padding: 20px;
            background-color: #f0f0f0;
        }
        .container {
            background-color: white;
            padding: 30px;
            border-radius: 10px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.1);
        }
        h1 {
            color: #333;
            text-align: center;
        }
        .info {
            background-color: #e7f3ff;
            padding: 15px;
            border-left: 4px solid #2196F3;
            margin: 20px 0;
        }
        .status {
            text-align: center;
            font-size: 18px;
            color: #4CAF50;
            font-weight: bold;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🎥 ESP32-CAM Web Server</h1>
        <div class="status">✓ Server is running!</div>
        <div class="info">
            <p><strong>Welcome to your ESP32-CAM web server!</strong></p>
            <p>The ESP32-CAM is connected and serving this page over WiFi.</p>
            <p>You can customize this HTML file to add camera streaming, controls, or any other functionality you need.</p>
        </div>
    </div>
</body>
</html>
)rawliteral";