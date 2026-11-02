// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape is \` for a backtick inside a comment.
export const VERSION = '#version 410 core\n';

// kCommon, source/Shaders.cpp
export const COMMON = `
//Where a monotonic piece crosses the horizontal line y. The same arithmetic
//as Outline.cpp's solveY, in float: the stable root of the quadratic in t
//that lands in [0, 1].
float crossingX( float y, vec2 p0, vec2 p1, vec2 p2, bool straight )
{
	float t;
	if( straight )
		t = ( y - p0.y ) / ( p2.y - p0.y );
	else
	{
		float a = p0.y - 2.0 * p1.y + p2.y;
		float b = 2.0 * ( p1.y - p0.y );
		float c = p0.y - y;
		if( abs( a ) <= 1.0e-6 * abs( b ) )
			t = -c / b;
		else
		{
			float s  = sqrt( max( b * b - 4.0 * a * c, 0.0 ) );
			float k  = -0.5 * ( b + ( b >= 0.0 ? s : -s ) );
			float t1 = k / a;
			float t2 = k != 0.0 ? c / k : t1;
			t        = t1 >= -1.0e-4 && t1 <= 1.0001 ? t1 : t2;
		}
	}
	t = clamp( t, 0.0, 1.0 );
	return mix( mix( p0.x, p1.x, t ), mix( p1.x, p2.x, t ), t );
}

float segmentDistance( vec2 q, vec2 a, vec2 b )
{
	vec2 d    = b - a;
	float len = dot( d, d );
	float t   = len > 0.0 ? clamp( dot( q - a, d ) / len, 0.0, 1.0 ) : 0.0;
	return length( a + d * t - q );
}

//The distance from q to a quadratic Bezier. |B(t) - q|^2 has derivative
//4 g(t) with g a cubic, and its minima are where g rises through zero: [0,1]
//is split where g turns (the roots of g', a quadratic), and each rise is
//bisected, then polished by Newton. Not the closed-form cubic: that divides
//by |p0 - 2 p1 + p2|^2 and goes wrong on a gently curved piece, which is
//most of a font.
float bezierDistance( vec2 q, vec2 p0, vec2 p1, vec2 p2 )
{
	vec2 a = p1 - p0, b = p0 - 2.0 * p1 + p2, d = p0 - q;
	float c3 = dot( b, b ), c2 = 3.0 * dot( a, b ), c1 = 2.0 * dot( a, a ) + dot( d, b ), c0 = dot( d, a );
	float qa = 3.0 * c3, qb = 2.0 * c2, qc = c1;
	float r0 = 2.0, r1 = 2.0;
	float disc = qb * qb - 4.0 * qa * qc;
	if( qa > 0.0 && disc >= 0.0 )
	{
		float s = sqrt( disc );
		float k = -0.5 * ( qb + ( qb >= 0.0 ? s : -s ) );
		r0      = k / qa;
		r1      = k != 0.0 ? qc / k : r0;
		if( r0 > r1 )
		{
			float swap = r0;
			r0         = r1;
			r1         = swap;
		}
	}
	float breaks[ 4 ] = float[ 4 ]( 0.0, 1.0, 1.0, 1.0 );
	int count         = 1;
	if( r0 > 0.0 && r0 < 1.0 )
		breaks[ count++ ] = r0;
	if( r1 > 0.0 && r1 < 1.0 )
		breaks[ count++ ] = r1;
	breaks[ count++ ] = 1.0;
	vec2 e     = p2 - q;
	float best = min( dot( d, d ), dot( e, e ) );
	for( int i = 0; i < 3; ++i )
	{
		if( i + 1 >= count )
			break;
		float lo = breaks[ i ], hi = breaks[ i + 1 ];
		float glo = ( ( c3 * lo + c2 ) * lo + c1 ) * lo + c0;
		float ghi = ( ( c3 * hi + c2 ) * hi + c1 ) * hi + c0;
		if( !( glo < 0.0 && ghi >= 0.0 ) )
			continue;
		for( int k = 0; k < 14; ++k )
		{
			float m  = 0.5 * ( lo + hi );
			float gm = ( ( c3 * m + c2 ) * m + c1 ) * m + c0;
			if( gm < 0.0 )
				lo = m;
			else
				hi = m;
		}
		float t = 0.5 * ( lo + hi );
		for( int k = 0; k < 3; ++k )
		{
			float g  = ( ( c3 * t + c2 ) * t + c1 ) * t + c0;
			float dg = ( 3.0 * c3 * t + 2.0 * c2 ) * t + c1;
			if( dg > 0.0 )
				t = clamp( t - g / dg, lo, hi );
		}
		vec2 r = d + ( 2.0 * a + b * t ) * t;
		best   = min( best, dot( r, r ) );
	}
	return sqrt( best );
}
`;

// kGlyphVertex, source/Shaders.cpp
export const GLYPH_VERTEX = `
uniform samplerBuffer Instances;
uniform vec2 Raster;
flat out vec2 glyphCentre;
flat out vec2 glyphAnchor;
flat out ivec2 glyphPieces;
flat out float glyphAlpha;
void main()
{
	int base    = gl_InstanceID * 3;
	vec4 rect   = texelFetch( Instances, base );
	vec4 place  = texelFetch( Instances, base + 1 );
	vec4 run    = texelFetch( Instances, base + 2 );
	vec2 corner = vec2( float( gl_VertexID & 1 ), float( gl_VertexID >> 1 ) );
	vec2 p      = mix( rect.xy, rect.zw, corner );
	gl_Position = vec4( p / Raster * 2.0 - 1.0, 0.0, 1.0 );
	glyphCentre = place.xy;
	glyphAlpha  = place.z;
	glyphPieces = ivec2( run.xy );
	glyphAnchor = run.zw;
}
`;

// kGlyphFragment, source/Shaders.cpp
export const GLYPH_FRAGMENT = `
uniform samplerBuffer Pieces;
uniform mat2 ToLocal;     //screen pixels to font units: rotation and scale
uniform vec2 FrameCentre; //pixels
uniform float UnitPx;     //pixels per font unit
uniform float Reach;      //font units: no piece further than this matters
uniform float Ramp;       //pixels: the edge's ramp, 1 + Feather
uniform float GrowPx;
uniform float OutlinePx;  //0 for filled letters
uniform int TestFlags;
flat in vec2 glyphCentre;
flat in vec2 glyphAnchor;
flat in ivec2 glyphPieces;
flat in float glyphAlpha;
out vec4 fragColour;

void main()
{
	//Relative to the anchor, the pixel and every piece alike: small numbers
	//near the frame's centre whatever the zoom (Knockout.cpp).
	vec2 q = ToLocal * ( gl_FragCoord.xy - FrameCentre ) + glyphCentre;
	int winding = 0, crossed = 0;
	float nearest = Reach;
	for( int i = 0; i < glyphPieces.y; ++i )
	{
		int at        = ( glyphPieces.x + i ) * 2;
		vec4 a        = texelFetch( Pieces, at );
		vec4 b        = texelFetch( Pieces, at + 1 );
		vec2 p0       = a.xy - glyphAnchor, p1 = a.zw - glyphAnchor, p2 = b.xy - glyphAnchor;
		int flags     = int( b.z );
		bool straight = ( flags & 1 ) != 0;
		//The winding: half-open in y, so a ray through a vertex counts once
		//and a ray grazing a top or a bottom counts zero times.
		if( p0.y != p2.y && q.y >= min( p0.y, p2.y ) && q.y < max( p0.y, p2.y ) )
		{
			if( crossingX( q.y, p0, p1, p2, straight ) > q.x )
			{
				winding += p2.y > p0.y ? 1 : -1;
				crossed += 1;
			}
		}
		//The distance, to boundary pieces only, and only to a piece whose
		//control box is nearer than the nearest so far.
		if( ( flags & 2 ) != 0 || ( TestFlags & 2 ) != 0 )
		{
			vec2 lo  = min( min( p0, p1 ), p2 ), hi = max( max( p0, p1 ), p2 );
			vec2 off = max( max( lo - q, q - hi ), vec2( 0.0 ) );
			if( dot( off, off ) < nearest * nearest )
				nearest = min( nearest, straight ? segmentDistance( q, p0, p2 ) : bezierDistance( q, p0, p1, p2 ) );
		}
	}
	bool inside = ( TestFlags & 1 ) != 0 ? ( crossed & 1 ) != 0 : winding != 0;
	float sd    = ( inside ? nearest : -nearest ) * UnitPx + GrowPx;
	float m;
	if( OutlinePx > 0.0 )
		m = clamp( 0.5 + ( 0.5 * OutlinePx - abs( sd ) ) / Ramp, 0.0, 1.0 );
	else
		m = clamp( 0.5 + sd / Ramp, 0.0, 1.0 );
	fragColour = vec4( m * glyphAlpha, 0.0, 0.0, 1.0 );
}
`;

// kQuadVertex, source/Shaders.cpp
export const QUAD_VERTEX = `
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;
out vec2 uv;
void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

// kCompositeFragment, source/Shaders.cpp
export const COMPOSITE_FRAGMENT = `
uniform sampler2D ClipIn;
uniform sampler2D MaskIn;
uniform int Outside;
uniform vec4 Ground;
uniform float MixAmount;
uniform vec2 ViewOrigin;
out vec4 fragColour;
void main()
{
	ivec2 p     = ivec2( gl_FragCoord.xy - ViewOrigin );
	vec4 clip   = texelFetch( ClipIn, p, 0 );
	float m     = texelFetch( MaskIn, p, 0 ).r;
	float k     = Outside == 1 ? 1.0 - m : m;
	vec4 masked = clip * k + Ground * ( 1.0 - k );
	fragColour  = mix( clip, masked, MixAmount );
}
`;

// Each program's stages, piece by piece, in the order the plugin joins them:
// the vertex stages as Knockout.cpp's InitGL builds them, the fragment
// stages as Shaders.cpp's Assemble() does.
export const PROGRAMS = {
  glyph: { vertex: ['VERSION', 'GLYPH_VERTEX'], fragment: ['VERSION', 'COMMON', 'GLYPH_FRAGMENT'] },
  composite: { vertex: ['VERSION', 'QUAD_VERTEX'], fragment: ['VERSION', 'COMMON', 'COMPOSITE_FRAGMENT'] },
};
