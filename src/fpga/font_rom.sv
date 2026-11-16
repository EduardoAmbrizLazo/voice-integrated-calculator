// 5x7 font, 64 glyphs (ASCII 0x20..0x5F), 8 column bytes per glyph (5 used),
// bit 0 = top pixel
module font_rom (
    input  logic       clk,
    input  logic [8:0] addr,      // {glyph[5:0], column[2:0]}
    output logic [7:0] q
);
    logic [7:0] rom [0:511];
    initial $readmemh("font.mem", rom);
    always_ff @(posedge clk) q <= rom[addr];
endmodule
