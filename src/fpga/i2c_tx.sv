// Write-only I2C master byte engine
module i2c_tx #(parameter int DIV = 16) (
    input  logic       clk, rst_n,
    input  logic       valid,
    input  logic [7:0] data,
    input  logic       last,
    output logic       ready,       // one clock: byte (and its ACK slot) finished
    input  logic       sda_i,
    output logic       scl_o, sda_o,
    output logic       nack         // slave did not acknowledge the latest byte
);
    typedef enum logic [3:0] {I_IDLE, I_STA1, I_STA2, I_B0, I_B1, I_B2, I_B3, I_HOLD,
                              I_STO1, I_STO2, I_STO3, I_STO4} st_t;
    st_t        st;
    logic [7:0] div;
    logic [7:0] sh;
    logic [3:0] nbit;               // 0..7 data, 8 = ACK slot
    logic       last_r;

    wire tick = (div == DIV - 1);
    assign ready = tick && (st == I_B3) && (nbit == 4'd8);

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            st <= I_IDLE; scl_o <= 1'b1; sda_o <= 1'b1; div <= '0; nack <= 1'b0;
        end else begin
            div <= tick ? 8'd0 : div + 8'd1;
            case (st)
                I_IDLE: begin
                    scl_o <= 1'b1; sda_o <= 1'b1;
                    if (valid) begin
                        sh <= data; last_r <= last; nbit <= 4'd0; div <= '0; st <= I_STA1;
                    end
                end
                I_STA1: begin sda_o <= 1'b0; if (tick) st <= I_STA2; end      // START
                I_STA2: begin scl_o <= 1'b0; if (tick) st <= I_B0; end
                I_B0: begin                                                   // SCL low: set SDA
                    scl_o <= 1'b0;
                    sda_o <= (nbit == 4'd8) ? 1'b1 : sh[7];
                    if (tick) st <= I_B1;
                end
                I_B1: begin scl_o <= 1'b1; if (tick) st <= I_B2; end          // SCL high
                I_B2: if (tick) begin
                    if (nbit == 4'd8) nack <= sda_i;
                    st <= I_B3;
                end
                I_B3: begin                                                   // SCL low
                    scl_o <= 1'b0;
                    if (tick) begin
                        if (nbit == 4'd8) st <= last_r ? I_STO1 : I_HOLD;
                        else begin nbit <= nbit + 4'd1; sh <= {sh[6:0], 1'b0}; st <= I_B0; end
                    end
                end
                I_HOLD: if (valid) begin                                      // SCL stays low
                    sh <= data; last_r <= last; nbit <= 4'd0; div <= '0; st <= I_B0;
                end
                I_STO1: begin scl_o <= 1'b0; sda_o <= 1'b0; if (tick) st <= I_STO2; end
                I_STO2: begin scl_o <= 1'b1; if (tick) st <= I_STO3; end      // STOP
                I_STO3: begin sda_o <= 1'b1; if (tick) st <= I_STO4; end
                I_STO4: if (tick) st <= I_IDLE;                               // bus free time
                default: st <= I_IDLE;
            endcase
        end
    end
endmodule
