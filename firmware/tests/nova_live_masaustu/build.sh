#!/bin/bash
# Masaüstü testi (Linux, g++): bağlantı mantığı sahte GPT-Live sunucusuyla (uyandırma, bakım, yazılı soru, veda, araya girme).
# Kullanım: ./build.sh [nova_live klasörü] && ./t_wake
H=$(cd "$(dirname "$0")" && pwd)
N=${1:-$H/../../nova_live}
g++ -std=gnu++17 -O1 -g -Wall -Wno-unused-function -DNOVA_HOST_TEST -I"$H" -I"$N" -o "$H/t_wake" \
  "$H/test_main.cpp" "$H/mocks.cpp" "$N/link.cpp" "$N/gptlive.cpp" "$N/wsclient.cpp" 2>&1 | grep -v "^$" | head -40
# Hafıza, zamanlayıcı, cihaz araçları ve sesle ses komutu: ./t_arac
g++ -std=gnu++17 -O1 -g -Wall -DNOVA_HOST_TEST -I"$H" -I"$N" -o "$H/t_arac" "$H/arac_test.cpp" "$N/hafiza.cpp" "$N/araclar.cpp" "$N/hava.cpp" "$N/notcikar.cpp" "$N/gundem.cpp" "$N/agistek.cpp" 2>&1 | grep -v "^$" | head -20
# Yankı silmenin donanımdan bağımsız parçaları: ./t_echo
g++ -std=gnu++17 -O2 -Wall -I"$N" -o "$H/t_echo" "$H/echo_test.cpp" 2>&1 | grep -v "^$" | head -20
# Ekran çizimi ve yazı tamponu (AddressSanitizer ile): ./t_ekran
g++ -std=gnu++17 -O1 -g -Wall -fsanitize=address,undefined -fno-omit-frame-pointer -I"$N" -o "$H/t_ekran" "$H/ekran_test.cpp" "$N/ekran.cpp" 2>&1 | grep -v "^$" | head -20
