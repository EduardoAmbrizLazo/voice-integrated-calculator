// SPI slave receiver, mode 0, MSB first
module spi_rx (
    input  logic       clk, rst_n,
    input  logic       sck, mosi, cs_n,
    output logic       valid,
    output logic       first,
    output logic [7:0] data
);
    logic [2:0] sck_s, cs_s;
    logic [1:0] mosi_s;
    logic [6:0] sh;
    logic [2:0] nbit;
    logic       is_first;

    always_ff @(posedge clk) begin
        sck_s  <= {sck_s[1:0], sck};
        cs_s   <= {cs_s[1:0], cs_n};
        mosi_s <= {mosi_s[0], mosi};
    end
    wire sck_rise = sck_s[1] & ~sck_s[2];

    always_ff @(posedge clk) begin
        valid <= 1'b0;
        if (!rst_n || cs_s[1]) begin
            nbit <= 3'd0;
            is_first <= 1'b1;
        end else if (sck_rise) begin
            sh   <= {sh[5:0], mosi_s[1]};
            nbit <= nbit + 3'd1;
            if (nbit == 3'd7) begin
                valid    <= 1'b1;
                data     <= {sh, mosi_s[1]};
                first    <= is_first;
                is_first <= 1'b0;
            end
        end
    end
endmodule
