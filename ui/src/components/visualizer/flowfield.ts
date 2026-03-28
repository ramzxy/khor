// Curl-noise flow field for particle advection.
// Uses 3D simplex noise sampled at (x, y, time) to produce a 2D
// divergence-free velocity field. Signal modulation controls turbulence.

// ── Simplex noise (inlined, public-domain Stefan Gustavson) ─────────────────

const F3 = 1 / 3, G3 = 1 / 6
const grad3: [number, number, number][] = [
  [1,1,0],[-1,1,0],[1,-1,0],[-1,-1,0],
  [1,0,1],[-1,0,1],[1,0,-1],[-1,0,-1],
  [0,1,1],[0,-1,1],[0,1,-1],[0,-1,-1],
]
const perm = new Uint8Array(512)
const pm12 = new Uint8Array(512)
const p = [151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180]
for (let i = 0; i < 512; i++) { perm[i] = p[i & 255]; pm12[i] = perm[i] % 12 }

function dot3(g: [number, number, number], x: number, y: number, z: number) {
  return g[0]*x + g[1]*y + g[2]*z
}

function simplex3(x: number, y: number, z: number): number {
  const s = (x+y+z)*F3
  const i = Math.floor(x+s), j = Math.floor(y+s), k = Math.floor(z+s)
  const t = (i+j+k)*G3
  const x0 = x-(i-t), y0 = y-(j-t), z0 = z-(k-t)
  let i1:number, j1:number, k1:number, i2:number, j2:number, k2:number
  if (x0>=y0) {
    if (y0>=z0) {i1=1;j1=0;k1=0;i2=1;j2=1;k2=0}
    else if (x0>=z0) {i1=1;j1=0;k1=0;i2=1;j2=0;k2=1}
    else {i1=0;j1=0;k1=1;i2=1;j2=0;k2=1}
  } else {
    if (y0<z0) {i1=0;j1=0;k1=1;i2=0;j2=1;k2=1}
    else if (x0<z0) {i1=0;j1=1;k1=0;i2=0;j2=1;k2=1}
    else {i1=0;j1=1;k1=0;i2=1;j2=1;k2=0}
  }
  const x1=x0-i1+G3, y1=y0-j1+G3, z1=z0-k1+G3
  const x2=x0-i2+2*G3, y2=y0-j2+2*G3, z2=z0-k2+2*G3
  const x3=x0-1+3*G3, y3=y0-1+3*G3, z3=z0-1+3*G3
  const ii=i&255, jj=j&255, kk=k&255
  const gi0=pm12[ii+perm[jj+perm[kk]]]
  const gi1=pm12[ii+i1+perm[jj+j1+perm[kk+k1]]]
  const gi2=pm12[ii+i2+perm[jj+j2+perm[kk+k2]]]
  const gi3=pm12[ii+1+perm[jj+1+perm[kk+1]]]
  let n0:number, n1:number, n2:number, n3:number
  let t0 = 0.6-x0*x0-y0*y0-z0*z0
  n0 = t0<0 ? 0 : (t0*=t0, t0*t0*dot3(grad3[gi0],x0,y0,z0))
  let t1 = 0.6-x1*x1-y1*y1-z1*z1
  n1 = t1<0 ? 0 : (t1*=t1, t1*t1*dot3(grad3[gi1],x1,y1,z1))
  let t2 = 0.6-x2*x2-y2*y2-z2*z2
  n2 = t2<0 ? 0 : (t2*=t2, t2*t2*dot3(grad3[gi2],x2,y2,z2))
  let t3 = 0.6-x3*x3-y3*y3-z3*z3
  n3 = t3<0 ? 0 : (t3*=t3, t3*t3*dot3(grad3[gi3],x3,y3,z3))
  return 32*(n0+n1+n2+n3)
}

// ── Flow field ──────────────────────────────────────────────────────────────

export interface FlowParams {
  frequency: number   // noise scale — higher = tighter curls
  speed: number       // time evolution rate
  strength: number    // force magnitude
  gravityStrength: number // pull toward center
}

/**
 * Sample the curl-noise flow field at a world-space position.
 * Returns [fx, fy] force vector.
 */
export function sampleFlow(
  x: number, y: number, time: number, params: FlowParams,
): [number, number] {
  const eps = 0.001
  const f = params.frequency
  const t = time * params.speed

  // Curl of 3D simplex noise projected to 2D: (dN/dy, -dN/dx)
  const dndx = (simplex3(x*f + eps, y*f, t) - simplex3(x*f - eps, y*f, t)) / (2 * eps)
  const dndy = (simplex3(x*f, y*f + eps, t) - simplex3(x*f, y*f - eps, t)) / (2 * eps)

  let fx = dndy * params.strength
  let fy = -dndx * params.strength

  // Gravity: gentle pull toward origin
  const dist = Math.sqrt(x*x + y*y) || 0.001
  const grav = params.gravityStrength / (1 + dist * 0.5) // softened falloff
  fx -= (x / dist) * grav
  fy -= (y / dist) * grav

  return [fx, fy]
}
