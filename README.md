# OpenCommander
USB Fan and temprature sensor managment firmware, application and board.

I have a linux server but the supermicro motherboard has limited fan headders and thermal zones. I wanted a much more flexible thermal management solution for it. 

Can manage 6 groups of 2 fans (12 total)
Up to 5 x DS18B20 temprature sensors
Posibillity to disable fan 5 and use pin as input from motherboard fan headder (yet to be implimented in firmware)

Emulates the corsair commander pro firmware so that the fans and tempratures show up in Linux using the in tree corsair module.
