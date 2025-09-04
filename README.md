# ESPcamImageServer

Sketch for ESP32-cam board that runs a webserver running a video stream at around 5fps

I brought a cheap ESP32-cam from aliexpress, installed the Arduino IDE, and worked with chatGPT to create this app

Install the ESP board support in the Arduino IDE board manager, and select the AI-Thinker board. I had to move the ESP32 board support in Arduino IDE down to 2.0.17 to get my board to work, 3.0.3 caused the board to boot-loop everytime I uploaded the code. I have used 10MHhz clocking of the camera sensor, 20Mhz didn't work at all for me. Flash the code at 115200.

Use the serial monitor at 115200 to see the IP address assigned to the ESP32, browse to the ip address on the network and you'll see the image stream at around 5fps plus some controls to set the "torch" white LED on/off/brightness