"""Erzeugt main/blob/<revision>.jtag.z aus dem Quartus-Ergebnis von as_release.

Ablauf: quartus_cpf macht aus output_files/<revision>.sof eine SVF. Daraus wird
der Datenstrom des SDR nach PROGRAM (0x002) gezogen -- in Schiebereihenfolge,
Byte k = Bits 8k..8k+7, Bit 0 zuerst --, mit zlib gepackt und abgelegt.

Warum nicht die .rbf: der JTAG-Strom ist 22 Byte 0xFF davor, die .rbf mit 15
geaenderten Kopf-Bytes (Optionsbits) und 4 Byte 0x00 danach. Geschoben wird
deshalb genau das, was Quartus selbst schieben wuerde.

Die Befehlsfolge um den Datenstrom herum ist in fpga_jtag.c fest eingebaut.
Dieses Skript prueft, dass die SVF genau diese Folge enthaelt, und bricht sonst ab.
Die Laenge des CHECK_STATUS-Registers und die Lage von CONF_DONE haengen vom Die
ab (EP4CE6: 603 / 226, EP4CE22: 732 / ...). Das Skript liest beides aus der SVF
und gibt es aus; die Werte gehoeren in die Tabelle RELEASES[] in fpga_jtag.c.

Revisionen (je eine .qsf in diesem Ordner, gleiche VHDL):
    as_release       EP4CE6F17C8   Board v7   -> main/blob/as_release.jtag.z
    as_release_e22   EP4CE22F17C8  Board v7.1 -> main/blob/as_release_e22.jtag.z

Aufruf (aus diesem Ordner, nach der Quartus-Uebersetzung der Revision):
    quartus_sh --flow compile as_release -c <revision>
    python make_jtag_blob.py [<revision>] [Pfad\\zu\\quartus_cpf.exe]
"""
import os
import re
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REV = sys.argv[1] if len(sys.argv) > 1 else 'as_release'
SOF = os.path.join(HERE, 'output_files', REV + '.sof')
OUT = os.path.join(HERE, '..', '..', 'main', 'blob', REV + '.jtag.z')
CPF = sys.argv[2] if len(sys.argv) > 2 else r'C:\intelFPGA_lite\22.1std\quartus\bin64\quartus_cpf.exe'

# Was fpga_jtag.c ausfuehrt; <SDR> steht fuer den Datenstrom.
EXPECTED = [
    'FREQUENCY 1.00E+07 HZ', 'TRST ABSENT', 'ENDDR IDLE', 'ENDIR IRPAUSE', 'STATE IDLE',
    'SIR 10 TDI (002)', 'RUNTEST IDLE 10000 TCK ENDSTATE IDLE', '<SDR>',
    'SIR 10 TDI (004)', 'RUNTEST 50 TCK', '<CHECK_STATUS>',
    'SIR 10 TDI (003)', 'RUNTEST 40960 TCK', 'RUNTEST 512 TCK',
    'SIR 10 TDI (3FF)', 'RUNTEST 10000 TCK', 'STATE IDLE',
]


def main():
    if not os.path.exists(SOF):
        sys.exit('%s fehlt -- erst "quartus_sh --flow compile as_release -c %s"' % (SOF, REV))
    with tempfile.TemporaryDirectory() as tmp:
        svf = os.path.join(tmp, REV + '.svf')
        subprocess.run([CPF, '-c', '-q', '10MHz', '-g', '3.3', '-n', 'p', SOF, svf],
                       check=True, stdout=subprocess.DEVNULL)
        txt = open(svf).read()

    txt = '\n'.join(l for l in txt.splitlines() if not l.startswith('!'))
    cmds = [' '.join(c.split()) for c in txt.split(';') if c.strip()]
    seq, data, nbits = [], None, 0
    cs_len, cs_bits = None, []
    for c in cmds:
        flat = c.replace(' ', '')
        m = re.match(r'SDR(\d+)TDI\(([0-9A-F]+)\)$', flat)
        cs = re.match(r'SDR(\d+)TDI\([0-9A-F]+\)(?:TDO\([0-9A-F]+\))?MASK\(([0-9A-F]+)\)$', flat)
        if m and int(m.group(1)) > 100000:
            nbits = int(m.group(1))
            data = int(m.group(2), 16).to_bytes((nbits + 7) // 8, 'little')
            seq.append('<SDR>')
        elif cs and seq and seq[-1] == 'RUNTEST 50 TCK':
            cs_len = int(cs.group(1))
            mask = int(cs.group(2), 16)
            cs_bits = [i for i in range(cs_len) if (mask >> i) & 1]
            seq.append('<CHECK_STATUS>')
        else:
            seq.append(c)
    if seq != EXPECTED:
        sys.exit('SVF-Ablauf weicht ab -- fpga_jtag.c pruefen:\n' + '\n'.join(seq))
    if nbits % 8:
        sys.exit('Datenstrom ist kein Vielfaches von 8 Bit: %d' % nbits)
    if len(cs_bits) != 1:
        sys.exit('CHECK_STATUS: erwartet genau ein MASK-Bit (CONF_DONE), gefunden %s' % cs_bits)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    packed = zlib.compress(data, 9)
    with open(OUT, 'wb') as f:
        f.write(packed)
    print('%s: %d Byte Datenstrom (%d Bit), gepackt %d Byte'
          % (os.path.normpath(OUT), len(data), nbits, len(packed)))
    print('CHECK_STATUS: %d Bit, CONF_DONE = Bit %d  (-> RELEASES[] in fpga_jtag.c)'
          % (cs_len, cs_bits[0]))


if __name__ == '__main__':
    main()
