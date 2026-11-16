// Small synchronous FIFO
module fifo #(parameter int W = 9, parameter int AW = 4) (
    input  logic         clk, rst_n,
    input  logic         push,
    input  logic [W-1:0] din,
    input  logic         pop,
    output logic         valid,
    output logic [W-1:0] dout
);
    logic [W-1:0] mem [0:(1<<AW)-1];
    logic [AW:0]  wp, rp;
    wire full = (wp[AW] != rp[AW]) && (wp[AW-1:0] == rp[AW-1:0]);

    assign valid = (wp != rp);
    assign dout  = mem[rp[AW-1:0]];

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            wp <= '0;
            rp <= '0;
        end else begin
            if (push && !full) begin
                mem[wp[AW-1:0]] <= din;
                wp <= wp + 1'b1;
            end
            if (pop && valid) rp <= rp + 1'b1;
        end
    end
endmodule
