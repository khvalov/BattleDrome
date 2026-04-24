# BattleDrome
Tank battle playground

Hybrid project of battle bots and online tanks. The idea to make an a platform to be used with physical equipement to play "tank battle". 

Wholce conecpt consist of next parts:
- Physical eqipment: Tanks, Turrets, Maybe drones some point. Majority are self-assembled eqipement based on arduino, rasberry, etc.  Each eqipment might have own properties can be changed over the game: Ammo, sppeed, armor, etc.
- Server to connect battefield eqipement. Basically server

#Technologies
- Most eqipement is Arduino driven. So C++ + Vibecoding
- Servers + server connectors - NodeJS (unless you want to do own)
- Raspberry to bridge main server and Arduino. I know it's overkill but have no better idea on how to run WiFi, modern stack lke NodeJs + MQTT on low-end eqipement without issues

#Folders
./eqipement - this where all eqipment resides. Assembling manuals, code, etc.
./server - well, this is what called server side. Dashboaard, eqipement set up, etc


#Contributing and using
No limits. This is pet project. Any contributor is velcome 

#support
This is community driven activity. Ask and someone will help..maybe.. 
