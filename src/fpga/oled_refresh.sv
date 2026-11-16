//Open Source OLED resresh
//SSD1306 refresh engine.  Every frame it sends the configuration commands
// (so a display that was unplugged or glitched recovers by itself), then all
// 1024 framebuffer bytes in horizontal addressing mode.  One frame takes about
// 25 ms.  Pages selected by blink_mask are blanked for 16 frames out of 32.
module oled_refresh #(
    parameter logic FLIP = 1'b0,           // 1 = rotate the picture 180 degrees
    parameter int   PWR_BITS = 22          // power-up wait = 2^PWR_BITS clocks (~175 ms)
) (
    input  logic       clk, rst_n,
    input  logic       invert,
    input  logic [7:0] blink_mask,
    output logic       rd_req,
    output logic [9:0] rd_addr,
    input  logic       rd_ok,
    input  logic [7:0] rd_data,
    output logic       tx_valid,
    output logic [7:0] tx_data,
    output logic       tx_last,
    input  logic       tx_ready
);
    localparam int NCMD = 33;
    function automatic logic [7:0] cmd(input logic [5:0] i);
        case (i)
            6'd0:  cmd = 8'h78;                 // I2C address 0x3C, write
            6'd1:  cmd = 8'h00;                 // control byte: commands follow
            6'd2:  cmd = 8'hD5; 6'd3:  cmd = 8'h80;      // clock divide
            6'd4:  cmd = 8'hA8; 6'd5:  cmd = 8'h3F;      // multiplex 64
            6'd6:  cmd = 8'hD3; 6'd7:  cmd = 8'h00;      // display offset 0
            6'd8:  cmd = 8'h40;                          // start line 0
            6'd9:  cmd = 8'h8D; 6'd10: cmd = 8'h14;      // charge pump on
            6'd11: cmd = 8'h20; 6'd12: cmd = 8'h00;      // horizontal addressing
            6'd13: cmd = FLIP ? 8'hA0 : 8'hA1;           // segment remap
            6'd14: cmd = FLIP ? 8'hC0 : 8'hC8;           // COM scan direction
            6'd15: cmd = 8'hDA; 6'd16: cmd = 8'h12;      // COM pins for 128x64
            6'd17: cmd = 8'h81; 6'd18: cmd = 8'hCF;      // contrast
            6'd19: cmd = 8'hD9; 6'd20: cmd = 8'hF1;      // pre-charge
            6'd21: cmd = 8'hDB; 6'd22: cmd = 8'h40;      // VCOMH level
            6'd23: cmd = 8'hA4;                          // show RAM contents
            6'd24: cmd = invert ? 8'hA7 : 8'hA6;         // inverse / normal
            6'd25: cmd = 8'h2E;                          // scrolling off
            6'd26: cmd = 8'hAF;                          // display on
            6'd27: cmd = 8'h21; 6'd28: cmd = 8'h00; 6'd29: cmd = 8'h7F;   // columns 0..127
            6'd30: cmd = 8'h22; 6'd31: cmd = 8'h00;                       // pages 0..7
            default: cmd = 8'h07;
        endcase
    endfunction

    typedef enum logic [2:0] {S_PWR, S_CMD, S_DH, S_RD, S_TX} state_t;
    state_t      state;
    logic [PWR_BITS-1:0] pwr_cnt;
    logic [5:0]  ci;
    logic        dh;
    logic [7:0]  pix;
    logic [4:0]  frame;

    wire blank = blink_mask[rd_addr[9:7]] & frame[4];

    always_comb begin
        tx_valid = 1'b0;
        tx_data  = 8'h00;
        tx_last  = 1'b0;
        rd_req   = (state == S_RD);
        case (state)
            S_CMD: begin tx_valid = 1'b1; tx_data = cmd(ci); tx_last = (ci == NCMD - 1); end
            S_DH:  begin tx_valid = 1'b1; tx_data = dh ? 8'h40 : 8'h78; end   // 0x40: data follows
            S_TX:  begin tx_valid = 1'b1; tx_data = blank ? 8'h00 : pix; tx_last = (rd_addr == 10'd1023); end
            default: ;
        endcase
    end

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            state <= S_PWR; pwr_cnt <= '0; ci <= '0; dh <= 1'b0; rd_addr <= '0; frame <= '0;
        end else begin
            case (state)
                S_PWR: begin
                    pwr_cnt <= pwr_cnt + 1'b1;
                    if (&pwr_cnt) state <= S_CMD;
                end
                S_CMD: if (tx_ready) begin
                    if (ci == NCMD - 1) begin ci <= '0; dh <= 1'b0; state <= S_DH; end
                    else ci <= ci + 6'd1;
                end
                S_DH: if (tx_ready) begin
                    if (dh) begin rd_addr <= '0; state <= S_RD; end
                    else dh <= 1'b1;
                end
                S_RD: if (rd_ok) begin pix <= rd_data; state <= S_TX; end
                S_TX: if (tx_ready) begin
                    if (rd_addr == 10'd1023) begin frame <= frame + 5'd1; state <= S_CMD; end
                    else begin rd_addr <= rd_addr + 10'd1; state <= S_RD; end
                end
                default: state <= S_PWR;
            endcase
        end
    end
endmodule
