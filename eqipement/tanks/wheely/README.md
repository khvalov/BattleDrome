# Wheely 
Mecanum wheels driven bot


# Source building construction kit: 
Original https://www.robotshop.com/products/makeblock-mbot-mega-robot-car-bluetooth-remote-controller 
I purchased ssame one but from eBay 

# Mandatoty parts for assembly: 
Electronic Components

1x MegaPi
2x DC motor driver
1x Bluetooth module
4x DC motor
2x RGB LED

Building Components
1x Body shell
1x 6 AA battery holder
4x Bracket
2x Pairs of 60 mm Mecanum wheel
1x Upper shell
68x Spacers/standoffs/ screws/nuts
4x Motor coupling

#Additional components: 
- Arduino nano
- Harnes for connecting Arduino. See below on 

# Assembling: 
Wire DC motors, bluetooth. 

# Arduino connection
I used MegaPI Raspberry port connected to 40 pins (10 pins used) header. Follow https://support.makeblock.com/hc/en-us/articles/1500012868722-Program-mBot-Mega-with-Raspberry-Pi-in-Python GPIO method. 
Since boards (Raspberry Nano) is not perfectly sized to MegaPI - I used 10 pins connector on MegaPI and flexible cable: 
- 10 Pin header: https://www.amazon.com/dp/B0F9NSTWCV?ref=ppx_yo2ov_dt_b_fed_asin_title
- Flex cable: https://www.amazon.com/dp/B01DP55PZQ?ref=ppx_yo2ov_dt_b_fed_asin_title
Note: this is not perfect fit on both ends.  Also make sure that 5V from MegaPI match 5V in on Raspberry side. I soildered 10 pin connected on upper side with key looks towrds the edge. 

# Base propertues: 
Speed: 0-255
Max armor: 100
Ammos: 100
Ammo power: 10
