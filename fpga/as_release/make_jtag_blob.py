"""Erzeugt main/blob/as_release.jtag.z aus dem Quartus-Ergebnis von as_release.

Ablauf: quartus_cpf macht aus output_files/as_release.sof eine SVF. Daraus wird
der Datenstrom des SDR nach PROGRAM (0x002) gezogen -- in Schiebereihenfolge,
Byte k = Bits 8k..8k+7, Bit 0 zuerst --, mit zlib gepackt und abgelegt.

Warum nicht die .rbf: der JTAG-Strom ist 22 Byte 0xFF davor, die .rbf mit 15
geaenderten Kopf-Bytes (Optionsbits) und 4 Byte 0x00 danach. Geschoben wird
deshalb genau das, was Quartus selbst schieben wuerde.

Die Befehlsfolge um den Datenstrom herum ist in fpga_prog.c fest eingebaut.
Dieses Skript prueft, dass die SVF genau diese Folge enthaelt, und bricht sonst ab.

Aufruf (aus diesem Ordner, nach der Quartus-Uebersetzung):
    python make_jtag_blob.py [Pfad\\zu\\quartus_cpf.exe]
"""
import os
import re
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SOF = os.path.join(HERE, 'output_files', 'as_release.sof')
OUT = os.path.join(HERE, '..', '..', 'main', 'blob', 'as_release.jtag.z')
CPF = sys.argv[1] if len(sys.argv) > 1 else r'C:\intelFPGA_lite\22.1std\quartus\bin64\quartus_cpf.exe'

# Was fpga_prog.c ausfuehrt; <SDR> steht fuer den Datenstrom.
EXPECTED = [
    'FREQUENCY 1.00E+07 HZ', 'TRST ABSENT', 'ENDDR IDLE', 'ENDIR IRPAUSE', 'STATE IDLE',
    'SIR 10 TDI (002)', 'RUNTEST IDLE 10000 TCK ENDSTATE IDLE', '<SDR>',
    'SIR 10 TDI (004)', 'RUNTEST 50 TCK', '<CHECK_STATUS>',
    'SIR 10 TDI (003)', 'RUNTEST 40960 TCK', 'RUNTEST 512 TCK',
    'SIR 10 TDI (3FF)', 'RUNTEST 10000 TCK', 'STATE IDLE',
]


def main():
    with tempfile.TemporaryDirectory() as tmp:
        svf = os.path.join(tmp, 'as_release.svf')
        subprocess.run([CPF, '-c', '-q', '10MHz', '-g', '3.3', '-n', 'p', SOF, svf],
                       check=True, stdout=subprocess.DEVNULL)
        txt = open(svf).read()

    txt = '\n'.join(l for l in txt.splitlines() if not l.startswith('!'))
    cmds = [' '.join(c.split()) for c in txt.split(';') if c.strip()]
    seq, data, nbits = [], None, 0
    for c in cmds:
        flat = c.replace(' ', '')
        m = re.match(r'SDR(\d+)TDI\(([0-9A-F]+)\)$', flat)
        if m and int(m.group(1)) > 100000:
            nbits = int(m.group(1))
            data = int(m.group(2), 16).to_bytes((nbits + 7) // 8, 'little')
            seq.append('<SDR>')
        elif flat.startswith('SDR603TDI('):
            seq.append('<CHECK_STATUS>')
        else:
            seq.append(c)
    if seq != EXPECTED:
        sys.exit('SVF-Ablauf weicht ab -- fpga_prog.c pruefen:\n' + '\n'.join(seq))
    if nbits % 8:
        sys.exit('Datenstrom ist kein Vielfaches von 8 Bit: %d' % nbits)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    packed = zlib.compress(data, 9)
    with open(OUT, 'wb') as f:
        f.write(packed)
    print('%s: %d Byte Datenstrom (%d Bit), gepackt %d Byte'
          % (os.path.normpath(OUT), len(data), nbits, len(packed)))


if __name__ == '__main__':
    main()
