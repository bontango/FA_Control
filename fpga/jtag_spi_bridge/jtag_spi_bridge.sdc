# TCK hoechstens 10 MHz (Cyclone IV tJCP >= 100 ns); FA_Control schiebt per Bit-Bang ~2 MHz.
create_clock -name tck -period 100 [get_ports altera_reserved_tck]
derive_clock_uncertainty
