// NoC types and helpers (SPEC.md, NoC packet format and Router).
//
// One plane is a 2D torus of one-way rings: NoC0 goes east (+x) and south (+y),
// NoC1 west (-x) and north (-y). Routing is X first. Every router computes the
// port its flit takes at the *next* router (lookahead), which travels on the link
// next to the flit.
package noc_pkg;

  localparam int unsigned COORD_W = 4;   // router grid up to 16x16
  typedef logic [COORD_W-1:0] coord_t;

  typedef enum logic [2:0] {
    OP_WRITE      = 3'd0,
    OP_READ_REQ   = 3'd1,
    OP_ATOMIC_INC = 3'd2,
    OP_MULTICAST  = 3'd3,  // reserved for M4
    OP_READ_RSP   = 3'd4   // a WRITE that answers a READ_REQ (counted by the requester)
  } op_e;

  // 204 bits, MSB first in the order of SPEC.md
  typedef struct packed {
    coord_t       dst_x;
    coord_t       dst_y;
    coord_t       src_x;
    coord_t       src_y;
    op_e          op;
    logic         vc;      // reserved
    logic [31:0]  addr;
    logic [15:0]  be;
    logic [7:0]   tag;
    logic [127:0] data;
  } flit_t;

  localparam int unsigned FLIT_W = $bits(flit_t);

  // Router ports, per plane: local, X ring, Y ring
  typedef enum logic [1:0] {
    P_LOCAL = 2'd0,
    P_X     = 2'd1,
    P_Y     = 2'd2
  } port_e;
  localparam int unsigned NPORTS = 3;
  // The same ports as integer indices into per-port arrays
  localparam int unsigned IL = 0, IX = 1, IY = 2;

  // Forward half of a link: flit, valid and the lookahead port. The backward
  // half is one credit wire (one credit per cycle at most).
  typedef struct packed {
    logic   valid;
    port_e  la_port;   // output port at the receiving router
    flit_t  flit;
  } link_t;

  // Credit round trip, in cycles, of a link with `slices` register slices in each
  // direction: send decision -> output register -> slices -> input buffer -> pop
  // -> credit register -> slices -> counter. An input buffer at least this deep
  // sustains one flit per cycle; router buffers add one slot for the bubble rule.
  function automatic int unsigned credit_rtt(int unsigned slices);
    return 4 + 2 * slices;
  endfunction

  // Next coordinate along a one-way ring: dir 0 = +1, dir 1 = -1, modulo n.
  function automatic coord_t ring_next(coord_t c, int unsigned n, bit dir);
    if (dir == 1'b0) return (int'(c) == int'(n) - 1) ? '0 : c + 1'b1;
    else             return (c == '0) ? coord_t'(n - 1) : c - 1'b1;
  endfunction

  // X-first route at router (x, y) towards (dx, dy)
  function automatic port_e route(coord_t x, coord_t y, coord_t dx, coord_t dy);
    if (x != dx) return P_X;
    if (y != dy) return P_Y;
    return P_LOCAL;
  endfunction

  // Hops from a to b on a one-way ring of length n going +1 (NoC0)
  function automatic int unsigned ring_dist(coord_t a, coord_t b, int unsigned n);
    return (b >= a) ? int'(b) - int'(a) : int'(b) + int'(n) - int'(a);
  endfunction

  // Plane of a flit from (sx, sy) to (dx, dy) on a gx x gy router grid, a fixed
  // function of source and destination (SPEC.md, plane selection): the plane
  // with fewer hops, ties to NoC0. 0 = NoC0, 1 = NoC1.
  function automatic logic select_plane(coord_t sx, coord_t sy, coord_t dx, coord_t dy,
                                        int unsigned gx, int unsigned gy);
    int unsigned hx, hy, h0, h1;
    hx = ring_dist(sx, dx, gx);
    hy = ring_dist(sy, dy, gy);
    h0 = hx + hy;
    h1 = (hx == 0 ? 0 : gx - hx) + (hy == 0 ? 0 : gy - hy);
    return h1 < h0;
  endfunction

endpackage
