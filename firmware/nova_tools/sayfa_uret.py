#!/usr/bin/env python3
# nova.local panelini (panel.html) firmware'e gömülecek başlık dosyasına çevirir: nova_live/panel_sayfa.h
# Kullanım: python3 sayfa_uret.py panel.html ../nova_live/panel_sayfa.h
import sys, re
kaynak, hedef = sys.argv[1], sys.argv[2]
s = open(kaynak, encoding='utf-8').read()
assert ')HTML"' not in s
# satır başı boşlukları ve boş satırları at (sayfa flaşta, her bayt istekte gider)
s = '\n'.join(l.strip() for l in s.splitlines() if l.strip())
open(hedef, 'w', encoding='utf-8').write(
    '// Otomatik üretildi: nova_tools/panel.html -> sayfa_uret.py. Elle değiştirme; panel.html\'i değiştirip yeniden üret.\n'
    '// netlog.cpp içinde, isimsiz ad alanının içinden eklenir.\n'
    'const char PAGE[] PROGMEM = R"HTML(' + s + ')HTML";\n')
print(hedef, len(s.encode()), 'bayt')
