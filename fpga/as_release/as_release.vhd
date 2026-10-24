-- as_release: Leer-Design fuer das EP4CE6 dev board v7 (EP4CE6F17C8).
--
-- Zweck: Das FPGA haelt DCLK auch bei nCONFIG low aktiv low (nCE liegt auf v7
-- fest auf GND), der ESP32 kommt so nicht an das EPCS. FA_Control laedt deshalb
-- zuerst dieses Design per JTAG in den SRAM. Es tut nichts und laesst die vier
-- AS-Leitungen als Eingaenge hochohmig; danach beschreibt der ESP das EPCS als
-- SPI-Master und startet das FPGA per nCONFIG-Puls aus dem neuen Image.
--
-- Die Ports existieren nur, damit die Pins als gewoehnliche Eingaenge
-- zugewiesen sind (dazu RESERVE_*_AFTER_CONFIGURATION = USE AS REGULAR IO in
-- der .qsf). Die Ausgabe liegt als as_release.rbf in output_files\, eingebettet
-- in die Firmware wird sie komprimiert (main/CMakeLists.txt).

library ieee;
use ieee.std_logic_1164.all;

entity as_release is
    port (
        dclk  : in std_logic;   -- H1
        asdo  : in std_logic;   -- C1
        ncso  : in std_logic;   -- D2
        data0 : in std_logic    -- H2
    );
end entity;

architecture rtl of as_release is
begin
end architecture;
