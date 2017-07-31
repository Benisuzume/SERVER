<b> SERVER INFO </b> <br/>
Re-Build: EXILEDNONAME <br/>
Version: 1.99.7.1.1 <br/>
<hr>

<b> ** CHANGELOG : </b><br/>
[+] Added do_botchat -- whisper text from username without "message_type_friendwhisperack". <br/>
[+] Fixed alias command "//" from "alias_command.cpp" <br/>
[+] Import new icon from warcraft3 to /icon in "account_wrap.cpp" and "icons-war3.bni". <br/>
[+] Added more commands to /clan in "clan.cpp", "clan.h", "handle_bnet.cpp", "command.cpp", "channel.cpp" : <br/>
----- Multi Chieftain. <br/>
----- Change message clan with rank clan. <br/>
----- Change member clan status "/clan [rank]". <br/>
----- Change your clan channel "/clan channel". <br/>
----- Kick your member clan "/clan kick". <br/>
<hr>

<b> ** MORE COMMANDS ( Connect to IRC-Bot ) : </b><br/>
[+] Added /botchatblue, /botchatred (null whisper text to target username with info & error). <br/>
[+] Added /rules, /donate, /event, /request, /online, /buy, /cash, /create, /host. <br/>
[+] Added /top10, /stats, /status, /dota, /lod, /imba. <br/>
[+] Added /chat, /accept, /decline, /lock, /unlock, /mute, /unmute. <br/>
<hr>

<b> ** INSTALLATION : </b><br/>
[+] sudo apt-get -y install build-essential zlib1g-dev git <br/>
[+] sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test <br/>
[+] sudo apt-get -y update <br/>
[+] sudo apt-get -y install gcc-5 g++-5 <br/>
[+] sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-5 60 --slave /usr/bin/g++ g++ /usr/bin/g++-5 <br/>
[+] sudo add-apt-repository -y ppa:george-edison55/cmake-3.x <br/>
[+] sudo apt-get update <br/>
[+] apt-get install liblua5.1-0-dev #Lua support <br/>
[+] apt-get install mysql-server mysql-client libmysqlclient-dev #MySQL support <br/>
[+] sudo apt-get -y install cmake <br/>
[+] git clone https://github.com/EXILEDNONAME/SERVER.git <br/>
[+] mkdir build <br/>
[+] cmake -D CMAKE_INSTALL_PREFIX=/usr/local/pvpgn -D WITH_MYSQL=true -D WITH_LUA=true ../ <br/>
[+] cd build && make <br/>
[+] make install <br/>
<hr>
