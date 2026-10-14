An Arduino + ESP32 machine that automatically weighs and packs a whole-spice masala mix (bay leaf, cinnamon, clove, sukumel, timur, cardamom) into small bags, with live monitoring and remote control over MQTT.
Features
Weight-based filling of 6 spices into one hopper using a load cell (HX711)
Fast feed followed by fine pulses near the target weight
Learns how much material is still falling after the motor stops, so accuracy improves each cycle
Jam and empty-hopper detection (stall timer)
Hopper trap door (servo) drops the mix into the bag; optional impulse sealer
Per-spice tolerance check, and each bag is flagged OK or out of tolerance
Settings (recipe, calibration, pack counter) saved in EEPROM
ESP32 gateway publishes events as JSON over MQTT and accepts remote commands
16x2 LCD, start button, bag sensor, buzzer, and hardware E-stop input
Spices (channels)
Ch	Spice	Suggested feeder
0	Tejpat (bay leaf)	Vibrating tray with agitator
1	Dalchini (cinnamon)	Vibrating tray or large vane
2	Long (clove)	Small auger
3	Sukumel	Small auger or slotted disc
4	Timur	Small auger or slotted disc
5	Alainchi (cardamom)	Vane wheel or wide auger

Names, target grams and tolerances can be changed in the code or with the SET command.

Hardware
Arduino Mega 2560 and ESP32 dev board
500 g load cell + HX711
6 DC gear motors with food-grade feeders, 6 logic-level MOSFETs (IRLZ44N) with flyback diodes
MG996R servo (separate 5-6 V 3 A supply)
IR obstacle sensor, 16x2 I2C LCD, push button, buzzer
Hardware E-stop that cuts motor power
12 V 5 A supply and 5 V buck converter
Optional: impulse sealer and relay module
Mega pin map
Function	Pin
Motor MOSFET gates	D2 D3 D4 D5 D6 D7
Hopper door servo	D9
HX711 DT / SCK	D30 / D31
Start button	D32 to GND
E-stop (NC switch)	D33 to GND
Bag IR sensor	D34 (LOW = bag present)
Sealer relay	D35 (active LOW)
Buzzer / LED	D36 / D37
LCD I2C	SDA 20, SCL 21 (address 0x27)
ESP32 link	Mega TX2/RX2 (16/17) to ESP32 GPIO16/17, common GND


# Masala-packing-automation-for-localbusiness-using-IOT
An comprensive IOT for masala business industry for packaging, Nepal remote places
