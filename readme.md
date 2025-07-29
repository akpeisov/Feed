Water
проект для анализа уровня воды в бочке и давлению на входе

Плата uniBoard
       ws1   ws2   ws3
IO36  IO13  IO33  IO32  GND  | +3.3V IO14 |  + -  |

       up    cen   dn

на IO36 датчик давления, делитель из 9.1к и 4.7к, т.е. для 5В должен подойти.
Уровень там 3.3В, 12 бит

IO36 adc
IO2 - датчик воды
IO33 - датчик воды
IO32 - датчик воды
IO14 - OW


Strapping Pins
The ESP32 chip has the following strapping pins:

GPIO 0 (must be LOW to enter boot mode)
GPIO 2 (must be floating or LOW during boot)
GPIO 4
GPIO 5 (must be HIGH during boot)
GPIO 12 (must be LOW during boot)
GPIO 15 (must be HIGH during boot)

GPIO 14 must be HIGH during boot а если датчик будет 0 показывать то жопа