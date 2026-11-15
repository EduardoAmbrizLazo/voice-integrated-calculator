//Arthur Cary 
//FPGA Display Driver
//Pipeline: MCU --> SPI--> spi_rx --> fifo --> renderer --> write--> SPRAM framebuffer 

module top (
    input  logic spi_sck,     // from MCU, mode 0, <= 1 MHz
    input  logic spi_mosi,
    input  logic spi_cs_n,
    inout  wire  oled_scl,    // I2C to the SSD1306 module (address 0x3C)
    inout  wire  oled_sda,
    output logic led          // high = OLED is not acknowledging (check wiring)
);
    // CLK + Power Reset 
    logic clk;
    HSOSC #(.CLKHF_DIV(2'b01)) osc (.CLKHFPU(1'b1), .CLKHFEN(1'b1), .CLKHF(clk));   // 24 MHz

    logic [7:0] por = 8'd0;
    logic       rst_n;
    always_ff @(posedge clk) if (!(&por)) por <= por + 8'd1;
    assign rst_n = &por;

    // SPI Reciever and FIFO 
    logic       rx_valid, rx_first;
    logic [7:0] rx_data;
    spi_rx u_rx (.clk, .rst_n, .sck(spi_sck), .mosi(spi_mosi), .cs_n(spi_cs_n),
                 .valid(rx_valid), .first(rx_first), .data(rx_data));

    logic       ff_valid, ff_pop;
    logic [8:0] ff_q;
    fifo #(.W(9), .AW(4)) u_fifo (.clk, .rst_n, .push(rx_valid), .din({rx_first, rx_data}),
                                  .pop(ff_pop), .valid(ff_valid), .dout(ff_q));

    // FPGA OK
    //FPGA Start up message, no MCU connection
    localparam int NBOOT = 11;
    logic [3:0] boot_i;
    logic [8:0] boot_q;
    always_comb
        case (boot_i)
          //notes
            4'd0:    boot_q = {1'b1, 8'h01};       // clear
            4'd1:    boot_q = {1'b1, 8'h02};       // text
            4'd2:    boot_q = {1'b0, 8'd22};       //   x
            4'd3:    boot_q = {1'b0, 8'h23};       //   page 3, scale 2
            4'd4:    boot_q = {1'b0, "F"};
            4'd5:    boot_q = {1'b0, "P"};
            4'd6:    boot_q = {1'b0, "G"};
            4'd7:    boot_q = {1'b0, "A"};
            4'd8:    boot_q = {1'b0, " "};
            4'd9:    boot_q = {1'b0, "O"};
            default: boot_q = {1'b0, "K"};
        endcase

    logic       booting, in_valid, in_pop;
    logic [8:0] in_q;
    assign booting  = (boot_i != NBOOT[3:0]);
    assign in_valid = booting ? 1'b1 : ff_valid;
    assign in_q     = booting ? boot_q : ff_q;
    assign ff_pop   = in_pop & ~booting;
    always_ff @(posedge clk)
        if (!rst_n)                 boot_i <= 4'd0;
        else if (booting && in_pop) boot_i <= boot_i + 4'd1;

    //Renderer
    logic [8:0] font_addr;
    logic [7:0] font_q;
    logic       wr_en, invert;
    logic [9:0] wr_addr;
    logic [7:0] wr_data, blink_mask;
    font_rom u_font (.clk, .addr(font_addr), .q(font_q));
    renderer u_rend (.clk, .rst_n, .in_valid, .in_first(in_q[8]), .in_data(in_q[7:0]), .in_pop,
                     .font_addr, .font_q, .fb_we(wr_en), .fb_waddr(wr_addr), .fb_wdata(wr_data),
                     .blink_mask, .invert);

    //Framebuffer
    // Writes from the renderer win; the refresh engine retries its read until granted.
    logic        rd_req, rd_ok;
    logic [9:0]  rd_addr;
    logic [15:0] ram_do;
    // read data is valid when rd_ok = 1
    always_ff @(posedge clk) rd_ok <= rd_req & ~wr_en;       

    SP256K u_fb (
        .CK(clk), .CS(1'b1), .WE(wr_en), .MASKWE(4'b1111),
        .AD({4'd0, (wr_en ? wr_addr : rd_addr)}), .DI({8'd0, wr_data}), .DO(ram_do),
        .STDBY(1'b0), .SLEEP(1'b0), .PWROFF_N(1'b1));

    //OLED resfresh to I2C
    logic       tx_valid, tx_last, tx_ready, nack, scl_o, sda_o;
    logic [7:0] tx_data;
    logic [1:0] sda_sync;
    always_ff @(posedge clk) sda_sync <= {sda_sync[0], oled_sda};

    oled_refresh u_ref (.clk, .rst_n, .invert, .blink_mask, .rd_req, .rd_addr, .rd_ok,
                        .rd_data(ram_do[7:0]), .tx_valid, .tx_data, .tx_last, .tx_ready);
    i2c_tx u_i2c (.clk, .rst_n, .valid(tx_valid), .data(tx_data), .last(tx_last), .ready(tx_ready),
                  .sda_i(sda_sync[1]), .scl_o, .sda_o, .nack);

    // open drain; the OLED module pull-ups
    assign oled_scl = scl_o ? 1'bz : 1'b0;     
    assign oled_sda = sda_o ? 1'bz : 1'b0;
    assign led = nack;
endmodule

