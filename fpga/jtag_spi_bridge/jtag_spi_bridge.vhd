-- jtag_spi_bridge: JTAG -> SPI zum Konfigurationsflash (EPCS), fuer den JTAG-Piggyback.
--
-- Zweck: Der Piggyback erreicht das FPGA nur ueber JTAG, das EPCS haengt aber an den
-- AS-Pins. FA_Control laedt dieses Design per JTAG in den SRAM und spricht das EPCS
-- dann durch den TAP an (ESP32-C3_EVALUIERUNG.md Abschnitt 14.4, Stufe B in
-- TECHNICAL_REFERENCE 15). Danach startet PULSE_NCONFIG das FPGA aus dem EPCS neu.
--
-- Protokoll: Solange ein Benutzerbefehl (FA_Control nimmt USER1, 0x00E) aktiv ist und der TAP in Shift-DR steht,
--   nCSO  = 0
--   DCLK  = TCK
--   ASDO  = TDI
--   TDO  <= DATA0
-- Jeder DR-Scan unter USER1 ist damit genau eine SPI-Transaktion (Befehl, Adresse,
-- Daten). Ausserhalb davon ist nCSO = 1 und DCLK = 0.
--
-- Die Freigabe wird auf der fallenden TCK-Flanke uebernommen (sel_q). Sonst kaeme beim
-- Eintritt in Shift-DR, waehrend TCK noch high ist, sofort eine steigende DCLK-Flanke.
-- Den Versatz DATA0 -> TDO bestimmt FA_Control je Sitzung an der JEDEC-Kennung
-- (fpga_prog.c calibrate()). Gemessen am v4.x (EP4CE6E22, 18.11.2026): Versatz 0.
--
-- Keine Benutzer-Pins: Die AS-Leitungen werden ueber das Atom cycloneive_asmiblock an
-- den festen Programmierpins angesprochen, die JTAG-Leitungen ueber cycloneive_jtag.
-- Deshalb sollte dasselbe Bitfile in allen Gehaeusen desselben Die laufen (EP4CE6/E10
-- E22 und F17, 10CL006/010). Bestaetigt bisher nur EP4CE6E22C8 (v4.x).
--
-- Erprobt am v4.x, 18.11.2026: Kennung 9D6015 (IS25LP016), READ BACK 512 kB bitgleich
-- zur GottFA80_v110_v305.jic, .jic mit ROMs geschrieben und verifiziert, PULSE_NCONFIG
-- startet das Spiel, CHECK_STATUS liefert CONF_DONE auch im Nutzerbetrieb.

library ieee;
use ieee.std_logic_1164.all;

entity jtag_spi_bridge is
    port (
        altera_reserved_tms : in  std_logic;
        altera_reserved_tck : in  std_logic;
        altera_reserved_tdi : in  std_logic;
        altera_reserved_tdo : out std_logic
    );
end entity;

architecture rtl of jtag_spi_bridge is
    -- Atome, von der Synthese als Primitive erkannt (Ports wie in
    -- quartus/eda/sim_lib/cycloneive_components.vhd). Eine library-Klausel kennt die
    -- Synthese nicht, deshalb die Deklaration hier.
    component cycloneive_jtag
        port (
            tms         : in  std_logic := '0';
            tck         : in  std_logic := '0';
            tdi         : in  std_logic := '0';
            -- tdoutap fehlt absichtlich: der Fitter lehnt den Port ab (Error 176304)
            tdouser     : in  std_logic := '0';
            tdo         : out std_logic;
            tmsutap     : out std_logic;
            tckutap     : out std_logic;
            tdiutap     : out std_logic;
            shiftuser   : out std_logic;
            clkdruser   : out std_logic;
            updateuser  : out std_logic;
            runidleuser : out std_logic;
            usr1user    : out std_logic
        );
    end component;

    component cycloneive_asmiblock
        port (
            dclkin   : in  std_logic;
            scein    : in  std_logic;
            oe       : in  std_logic;
            sdoin    : in  std_logic;
            data0out : out std_logic
        );
    end component;

    signal tck, tdi, shiftuser, usr1 : std_logic;
    signal sel, sel_q                : std_logic := '0';
    signal dclk, nce, data0          : std_logic;
    signal tdi_q, tdo_u              : std_logic := '0';
begin
    jtag : cycloneive_jtag
        port map (
            tms         => altera_reserved_tms,
            tck         => altera_reserved_tck,
            tdi         => altera_reserved_tdi,
            tdo         => altera_reserved_tdo,
            tdouser     => tdo_u,
            tmsutap     => open,
            tckutap     => tck,
            tdiutap     => tdi,
            shiftuser   => shiftuser,
            clkdruser   => open,
            updateuser  => open,
            runidleuser => open,
            usr1user    => usr1
        );

    -- Jeder Datenscan unter USER0 oder USER1 (ohne Hub gibt es keinen anderen Nutzer).
    -- usr1user wird bewusst nicht ausgewertet: Der Hub meint damit seinen Adressscan,
    -- die Bedeutung am rohen Atom ist nicht belegt (erster Test 18.11.2026: keine
    -- Antwort mit "shiftuser and usr1user").
    sel <= shiftuser;

    process (tck)
    begin
        if falling_edge(tck) then
            sel_q <= sel;
        end if;
    end process;

    -- Diagnose (fpga_jtag_bridge_diag): unter USER1 (usr1user = 1) geht DATA0 an TDO,
    -- sonst -- also unter USER0 -- TDI um einen Takt verzoegert (Echo). So zeigt
    -- /api/fpgadiag getrennt, ob der Rueckweg durch das Atom ueberhaupt traegt und ob
    -- usr1user je high wird. FA_Control schiebt die SPI-Daten unter USER1.
    process (tck)
    begin
        if rising_edge(tck) then
            tdi_q <= tdi;
        end if;
    end process;
    tdo_u <= data0 when usr1 = '1' else tdi_q;

    dclk <= tck and sel_q;
    nce  <= not sel_q;

    asmi : cycloneive_asmiblock
        port map (
            dclkin   => dclk,
            scein    => nce,
            oe       => '0',          -- 0 = die AS-Pins gehoeren diesem Design
            sdoin    => tdi,
            data0out => data0
        );
end architecture;
