// Command parser and glyph renderer.
module renderer (
    input  logic       clk, rst_n,
    input  logic       in_valid,
    input  logic       in_first,
    input  logic [7:0] in_data,
    output logic       in_pop,
    output logic [8:0] font_addr,
    input  logic [7:0] font_q,
    output logic       fb_we,
    output logic [9:0] fb_waddr,
    output logic [7:0] fb_wdata,
    output logic [7:0] blink_mask,
    output logic       invert
);
    typedef enum logic [3:0] {S_CLR, S_IDLE, S_X, S_CFG, S_CHAR, S_FETCH, S_WRITE, S_RAW,
                              S_BLINK, S_INV} state_t;
    state_t     state;
    logic       raw;
    logic [7:0] x;            // bit 7 set = off the right edge, writes suppressed
    logic [2:0] page;
    logic [1:0] scale;        // 1..3
    logic [5:0] glyph;
    logic [2:0] col;          // 0..5 (column 5 is the blank gap, stored as 0 in the ROM)
    logic [1:0] xrep, prow;
    logic [9:0] clr_addr;

    // ASCII -> glyph index; lower case folds to upper case, anything else is a space
    function automatic logic [5:0] fold(input logic [7:0] c);
        logic [7:0] u;
        u = (c >= 8'h60 && c <= 8'h7F) ? (c - 8'h20) : c;
        if (u < 8'h20 || u > 8'h5F) fold = 6'd0;
        else                        fold = u[5:0] - 6'd32;
    endfunction

    // stretch 7 font rows vertically by the scale factor (row r -> rows r*sc .. r*sc+sc-1)
    function automatic logic [23:0] stretch(input logic [6:0] g, input logic [1:0] sc);
        logic [23:0] s1, s2, s3;
        s1 = {17'd0, g};
        s2 = '0;
        s3 = '0;
        for (int r = 0; r < 7; r++) begin
            s2[2*r]   = g[r];
            s2[2*r+1] = g[r];
            s3[3*r]   = g[r];
            s3[3*r+1] = g[r];
            s3[3*r+2] = g[r];
        end
        stretch = (sc == 2'd3) ? s3 : (sc == 2'd2) ? s2 : s1;
    endfunction

    logic [23:0] tall;
    logic [3:0]  pg;
    assign font_addr = {glyph, col};
    assign tall      = stretch(font_q[6:0], scale);
    assign pg        = {1'b0, page} + {2'b00, prow};

    wire arg = in_valid & ~in_first;        // an argument byte (a new opcode aborts instead)

    always_comb begin
        in_pop   = 1'b0;
        fb_we    = 1'b0;
        fb_waddr = '0;
        fb_wdata = '0;
        case (state)
            S_CLR: begin
                fb_we    = 1'b1;
                fb_waddr = clr_addr;
            end
            S_IDLE: in_pop = in_valid;
            S_X, S_CFG, S_CHAR, S_BLINK, S_INV: in_pop = arg;
            S_RAW: begin
                in_pop   = arg;
                fb_we    = arg & ~x[7];
                fb_waddr = {page, x[6:0]};
                fb_wdata = in_data;
            end
            S_WRITE: begin
                fb_we    = ~x[7] & ~pg[3];
                fb_waddr = {pg[2:0], x[6:0]};
                fb_wdata = (prow == 2'd0) ? tall[7:0] : (prow == 2'd1) ? tall[15:8] : tall[23:16];
            end
            default: ;
        endcase
    end

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            state      <= S_CLR;          // SPRAM powers up undefined: clear it first
            clr_addr   <= '0;
            blink_mask <= '0;
            invert     <= 1'b0;
        end else begin
            case (state)
                S_CLR: begin
                    clr_addr <= clr_addr + 10'd1;
                    if (clr_addr == 10'd1023) state <= S_IDLE;
                end
                S_IDLE: if (in_valid && in_first)
                    case (in_data)
                        8'h01: begin clr_addr <= '0; state <= S_CLR; end
                        8'h02: begin raw <= 1'b0; state <= S_X; end
                        8'h05: begin raw <= 1'b1; state <= S_X; end
                        8'h03: state <= S_BLINK;
                        8'h04: state <= S_INV;
                        default: ;
                    endcase
                S_X: if (in_valid) begin
                    if (in_first) state <= S_IDLE;
                    else begin x <= in_data; state <= S_CFG; end
                end
                S_CFG: if (in_valid) begin
                    if (in_first) state <= S_IDLE;
                    else begin
                        page  <= in_data[2:0];
                        scale <= (in_data[5:4] == 2'd0) ? 2'd1 : in_data[5:4];
                        state <= raw ? S_RAW : S_CHAR;
                    end
                end
                S_CHAR: if (in_valid) begin
                    if (in_first) state <= S_IDLE;
                    else begin
                        glyph <= fold(in_data);
                        col   <= 3'd0;
                        xrep  <= 2'd0;
                        prow  <= 2'd0;
                        state <= S_FETCH;
                    end
                end
                S_FETCH: state <= S_WRITE;          // font ROM has one cycle of latency
                S_WRITE: begin
                    if (prow != scale - 2'd1) prow <= prow + 2'd1;
                    else begin
                        prow <= 2'd0;
                        if (!x[7]) x <= x + 8'd1;
                        if (xrep != scale - 2'd1) xrep <= xrep + 2'd1;
                        else begin
                            xrep <= 2'd0;
                            if (col == 3'd5) state <= S_CHAR;
                            else begin col <= col + 3'd1; state <= S_FETCH; end
                        end
                    end
                end
                S_RAW: if (in_valid) begin
                    if (in_first) state <= S_IDLE;
                    else if (!x[7]) x <= x + 8'd1;
                end
                S_BLINK: if (in_valid) begin
                    if (!in_first) blink_mask <= in_data;
                    state <= S_IDLE;
                end
                S_INV: if (in_valid) begin
                    if (!in_first) invert <= in_data[0];
                    state <= S_IDLE;
                end
                default: state <= S_IDLE;
            endcase
        end
    end
endmodule
