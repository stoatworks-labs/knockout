#include "Outline.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace knockout
{
namespace
{
Vec2 operator+( Vec2 a, Vec2 b )
{
	return { a.x + b.x, a.y + b.y };
}
Vec2 operator-( Vec2 a, Vec2 b )
{
	return { a.x - b.x, a.y - b.y };
}
Vec2 operator*( Vec2 a, double s )
{
	return { a.x * s, a.y * s };
}
bool operator==( Vec2 a, Vec2 b )
{
	return a.x == b.x && a.y == b.y;
}
bool operator!=( Vec2 a, Vec2 b )
{
	return !( a == b );
}
double dot( Vec2 a, Vec2 b )
{
	return a.x * b.x + a.y * b.y;
}
double cross( Vec2 a, Vec2 b )
{
	return a.x * b.y - a.y * b.x;
}
double length( Vec2 a )
{
	return std::sqrt( dot( a, a ) );
}
Vec2 lerp( Vec2 a, Vec2 b, double t )
{
	return { a.x + ( b.x - a.x ) * t, a.y + ( b.y - a.y ) * t };
}
Vec2 midpoint( Vec2 a, Vec2 b )
{
	return { ( a.x + b.x ) * 0.5, ( a.y + b.y ) * 0.5 };
}

Piece makeLine( Vec2 a, Vec2 b )
{
	Piece p;
	p.p0   = a;
	p.p1   = midpoint( a, b );
	p.p2   = b;
	p.line = true;
	return p;
}

/// The quadratic's control point for the stretch [a, b] of its parameter:
/// its polar form at (a, b). Exact, so a stretch's control is the same
/// however many times the piece was split before it.
Vec2 blossom( const Piece& p, double a, double b )
{
	const double w0 = ( 1.0 - a ) * ( 1.0 - b ), w1 = ( 1.0 - a ) * b + a * ( 1.0 - b ), w2 = a * b;
	return { w0 * p.p0.x + w1 * p.p1.x + w2 * p.p2.x, w0 * p.p0.y + w1 * p.p1.y + w2 * p.p2.y };
}

/// A stretch of a piece, between end points the caller computed once, so
/// two neighbours share theirs bit for bit and the contour stays closed.
Piece stretch( const Piece& p, double a, double b, Vec2 start, Vec2 end )
{
	Piece s;
	s.p0       = start;
	s.p2       = end;
	s.line     = p.line;
	s.p1       = p.line ? midpoint( start, end ) : blossom( p, a, b );
	s.boundary = p.boundary;
	return s;
}

/// A quadratic that strays less than `tolerance` from its chord, and does
/// not run past either end of it, is drawn and measured as the chord.
bool nearlyStraight( Vec2 p0, Vec2 p1, Vec2 p2, double tolerance )
{
	const Vec2 chord  = p2 - p0;
	const double span = length( chord );
	if( span == 0.0 )
		return false;
	//The curve's furthest point from the chord is half its control's.
	const double straying = 0.5 * std::fabs( cross( chord, p1 - p0 ) ) / span;
	const double along    = dot( p1 - p0, chord ) / ( span * span );
	return straying <= tolerance && along >= 0.0 && along <= 1.0;
}

/// Split a quadratic at its y-extremum, if it has one inside (0, 1). The
/// halves' controls lie on the extremum's horizontal tangent; they are set
/// to its y exactly so neither half turns back in y after rounding.
void pushMonotonic( const Piece& p, std::vector< Piece >& out )
{
	if( p.line )
	{
		out.push_back( p );
		return;
	}
	const double lo = std::min( p.p0.y, p.p2.y ), hi = std::max( p.p0.y, p.p2.y );
	const double denominator = p.p0.y - 2.0 * p.p1.y + p.p2.y;
	if( denominator != 0.0 )
	{
		const double t = ( p.p0.y - p.p1.y ) / denominator;
		if( t > 1.0e-9 && t < 1.0 - 1.0e-9 )
		{
			const Vec2 m = Evaluate( p, t );
			Piece a      = p, b = p;
			a.p1         = lerp( p.p0, p.p1, t );
			a.p2         = m;
			b.p0         = m;
			b.p1         = lerp( p.p1, p.p2, t );
			a.p1.y = b.p1.y = m.y;
			out.push_back( a );
			out.push_back( b );
			return;
		}
	}
	Piece q = p;
	q.p1.y  = std::clamp( q.p1.y, lo, hi );
	out.push_back( q );
}

struct Box
{
	double x0, y0, x1, y1;
};

Box controlBox( const Piece& p )
{
	return { std::min( { p.p0.x, p.p1.x, p.p2.x } ), std::min( { p.p0.y, p.p1.y, p.p2.y } ),
		     std::max( { p.p0.x, p.p1.x, p.p2.x } ), std::max( { p.p0.y, p.p1.y, p.p2.y } ) };
}

bool overlaps( const Box& a, const Box& b, double slack )
{
	return a.x0 <= b.x1 + slack && b.x0 <= a.x1 + slack && a.y0 <= b.y1 + slack && b.y0 <= a.y1 + slack;
}

/// The parameter at which a monotonic piece reaches height y. The same
/// arithmetic the shader does, in double.
double solveY( const Piece& p, double y )
{
	if( p.line )
		return std::clamp( ( y - p.p0.y ) / ( p.p2.y - p.p0.y ), 0.0, 1.0 );
	const double a = p.p0.y - 2.0 * p.p1.y + p.p2.y;
	const double b = 2.0 * ( p.p1.y - p.p0.y );
	const double c = p.p0.y - y;
	double t;
	if( std::fabs( a ) <= 1.0e-12 * ( std::fabs( b ) + 1.0e-300 ) )
		t = -c / b;
	else
	{
		const double s  = std::sqrt( std::max( b * b - 4.0 * a * c, 0.0 ) );
		const double q  = -0.5 * ( b + ( b >= 0.0 ? s : -s ) );
		const double t1 = q / a;
		const double t2 = q != 0.0 ? c / q : t1;
		t               = t1 >= -1.0e-9 && t1 <= 1.0 + 1.0e-9 ? t1 : t2;
	}
	return std::clamp( t, 0.0, 1.0 );
}

struct Hit
{
	double t, s;
};

/// Where two pieces cross, by subdividing the longer until both are
/// smaller than `size`. A pair that never separates within the budget is
/// two coincident stretches of outline, and has no crossing to split at.
bool subdivide( const Piece& a, double a0, double a1, const Piece& b, double b0, double b1, double size, int& budget,
                std::vector< Hit >& hits )
{
	if( --budget < 0 )
		return false;
	const Piece sa = stretch( a, a0, a1, Evaluate( a, a0 ), Evaluate( a, a1 ) );
	const Piece sb = stretch( b, b0, b1, Evaluate( b, b0 ), Evaluate( b, b1 ) );
	const Box ba = controlBox( sa ), bb = controlBox( sb );
	if( !overlaps( ba, bb, 0.0 ) )
		return true;
	const double extentA = std::max( ba.x1 - ba.x0, ba.y1 - ba.y0 ), extentB = std::max( bb.x1 - bb.x0, bb.y1 - bb.y0 );
	if( extentA < size && extentB < size )
	{
		hits.push_back( { 0.5 * ( a0 + a1 ), 0.5 * ( b0 + b1 ) } );
		return true;
	}
	if( extentA >= extentB )
	{
		const double m = 0.5 * ( a0 + a1 );
		return subdivide( a, a0, m, b, b0, b1, size, budget, hits ) && subdivide( a, m, a1, b, b0, b1, size, budget, hits );
	}
	const double m = 0.5 * ( b0 + b1 );
	return subdivide( a, a0, a1, b, b0, m, size, budget, hits ) && subdivide( a, a0, a1, b, m, b1, size, budget, hits );
}

Vec2 derivative( const Piece& p, double t )
{
	return ( ( p.p1 - p.p0 ) * ( 1.0 - t ) + ( p.p2 - p.p1 ) * t ) * 2.0;
}

/// Newton on A(t) = B(s), from a subdivision hit: it lands the split on
/// both pieces at the same point to well under the subdivision's size.
Hit polish( const Piece& a, const Piece& b, Hit h )
{
	for( int i = 0; i < 6; ++i )
	{
		const Vec2 r  = Evaluate( a, h.t ) - Evaluate( b, h.s );
		const Vec2 da = derivative( a, h.t ), db = derivative( b, h.s );
		//Solve da * dt - db * ds = -r.
		const double det = -cross( da, db );
		if( std::fabs( det ) < 1.0e-18 )
			break;
		const double dt = ( -r.x * -db.y - -db.x * -r.y ) / det;
		const double ds = ( da.x * -r.y - da.y * -r.x ) / det;
		const Hit next  = { h.t + dt, h.s + ds };
		if( next.t < 0.0 || next.t > 1.0 || next.s < 0.0 || next.s > 1.0 )
			break;
		h = next;
	}
	return h;
}

std::vector< Hit > crossings( const Piece& a, const Piece& b, double size )
{
	std::vector< Hit > hits;
	if( a.line && b.line )
	{
		const Vec2 da = a.p2 - a.p0, db = b.p2 - b.p0;
		const double det = cross( da, db );
		//Parallel, or collinear and overlapping: nothing to split at.
		if( std::fabs( det ) <= 1.0e-12 * length( da ) * length( db ) )
			return hits;
		const Vec2 w   = b.p0 - a.p0;
		const double t = cross( w, db ) / det, s = cross( w, da ) / det;
		if( t >= 0.0 && t <= 1.0 && s >= 0.0 && s <= 1.0 )
			hits.push_back( { t, s } );
		return hits;
	}
	int budget = 20000;
	if( !subdivide( a, 0.0, 1.0, b, 0.0, 1.0, size, budget, hits ) )
		return {};
	//One crossing leaves a cluster of neighbouring leaves: one hit each.
	std::vector< Hit > merged;
	for( const Hit& h : hits )
	{
		bool near = false;
		for( Hit& m : merged )
			if( length( Evaluate( a, h.t ) - Evaluate( a, m.t ) ) < 8.0 * size )
			{
				near = true;
				break;
			}
		if( !near )
			merged.push_back( h );
	}
	for( Hit& h : merged )
		h = polish( a, b, h );
	return merged;
}

/// The worst distance from `quad` to the cubic, as a 64-chord polyline:
/// the half of the deviation that sampling the cubic alone would miss.
double quadToCubicDeviation( Vec2 p0, Vec2 c1, Vec2 c2, Vec2 p3, const Piece& quad )
{
	auto cubic = [ & ]( double t ) {
		const double u = 1.0 - t;
		return p0 * ( u * u * u ) + c1 * ( 3.0 * u * u * t ) + c2 * ( 3.0 * u * t * t ) + p3 * ( t * t * t );
	};
	std::vector< Piece > chords;
	Vec2 previous = p0;
	for( int i = 1; i <= 64; ++i )
	{
		const Vec2 next = cubic( i / 64.0 );
		chords.push_back( makeLine( previous, next ) );
		previous = next;
	}
	double worst = 0.0;
	for( int i = 1; i < 32; ++i )
	{
		const Vec2 q = Evaluate( quad, i / 32.0 );
		double best  = std::numeric_limits< double >::max();
		for( const Piece& c : chords )
			best = std::min( best, DistanceToPiece( c, q ) );
		worst = std::max( worst, best );
	}
	return worst;
}

/// The deviation is sampled, and the worst of it falls between samples: a
/// candidate is accepted at 0.8 of the tolerance so the promise holds when
/// it is measured densely (the harness does, at 513 points).
constexpr double kAccept = 0.8;

void approximateCubic( Vec2 p0, Vec2 c1, Vec2 c2, Vec2 p3, double tolerance, int depth, std::vector< Piece >& out )
{
	//The quadratic that leaves and arrives along the cubic's own tangents:
	//its control is where they meet. It keeps the joins between pieces
	//smooth, which a least-squares fit does not.
	Vec2 d0 = c1 - p0, d3 = p3 - c2;
	if( length( d0 ) < 1.0e-12 )
		d0 = c2 - p0;
	if( length( d3 ) < 1.0e-12 )
		d3 = p3 - c1;
	Piece candidate;
	candidate.p0 = p0;
	candidate.p2 = p3;
	bool tangent = false;
	const double det = cross( d0, d3 );
	if( std::fabs( det ) > 1.0e-12 * length( d0 ) * length( d3 ) )
	{
		const Vec2 w   = p3 - p0;
		const double u = cross( w, d3 ) / det, v = cross( d0, w ) / det;
		if( u > 0.0 && v > 0.0 )
		{
			candidate.p1 = p0 + d0 * u;
			tangent      = true;
		}
	}
	if( !tangent )
		candidate.p1 = ( ( c1 + c2 ) * 3.0 - ( p0 + p3 ) ) * 0.25;

	double error = 0.0;
	if( tangent || depth >= 10 )
	{
		auto cubic = [ & ]( double t ) {
			const double u = 1.0 - t;
			return p0 * ( u * u * u ) + c1 * ( 3.0 * u * u * t ) + c2 * ( 3.0 * u * t * t ) + p3 * ( t * t * t );
		};
		for( int i = 1; i < 32; ++i )
			error = std::max( error, DistanceToPiece( candidate, cubic( i / 32.0 ) ) );
		if( error <= kAccept * tolerance )
			error = std::max( error, quadToCubicDeviation( p0, c1, c2, p3, candidate ) );
	}
	else
		error = std::numeric_limits< double >::max();
	if( error <= kAccept * tolerance || depth >= 10 )
	{
		out.push_back( candidate );
		return;
	}
	//de Casteljau at a half.
	const Vec2 a = midpoint( p0, c1 ), b = midpoint( c1, c2 ), c = midpoint( c2, p3 );
	const Vec2 d = midpoint( a, b ), e = midpoint( b, c );
	const Vec2 m = midpoint( d, e );
	approximateCubic( p0, a, d, m, tolerance, depth + 1, out );
	approximateCubic( m, e, c, p3, tolerance, depth + 1, out );
}
} // namespace

//---------------------------------------------------------------------------
Vec2 Evaluate( const Piece& p, double t )
{
	const double u = 1.0 - t;
	return { u * u * p.p0.x + 2.0 * u * t * p.p1.x + t * t * p.p2.x, u * u * p.p0.y + 2.0 * u * t * p.p1.y + t * t * p.p2.y };
}

double DistanceToPiece( const Piece& p, Vec2 q )
{
	if( p.line )
	{
		const Vec2 d     = p.p2 - p.p0;
		const double len = dot( d, d );
		const double t   = len > 0.0 ? std::clamp( dot( q - p.p0, d ) / len, 0.0, 1.0 ) : 0.0;
		return length( p.p0 + d * t - q );
	}
	//|B(t) - q|^2 has derivative 4 g(t), g a cubic: its minima are where g
	//rises through zero. Split [0, 1] where g turns, bisect each rise.
	const Vec2 a = p.p1 - p.p0, b = p.p0 - p.p1 * 2.0 + p.p2, d = p.p0 - q;
	const double c3 = dot( b, b ), c2 = 3.0 * dot( a, b ), c1 = 2.0 * dot( a, a ) + dot( d, b ), c0 = dot( d, a );
	auto g = [ & ]( double t ) { return ( ( c3 * t + c2 ) * t + c1 ) * t + c0; };
	double breaks[ 4 ] = { 0.0, 1.0, 1.0, 1.0 };
	int count          = 1;
	{
		//g' = 3 c3 t^2 + 2 c2 t + c1.
		const double qa = 3.0 * c3, qb = 2.0 * c2, qc = c1;
		double roots[ 2 ];
		int found = 0;
		if( std::fabs( qa ) < 1.0e-300 )
		{
			if( std::fabs( qb ) > 1.0e-300 )
				roots[ found++ ] = -qc / qb;
		}
		else
		{
			const double disc = qb * qb - 4.0 * qa * qc;
			if( disc >= 0.0 )
			{
				const double s = std::sqrt( disc );
				const double k = -0.5 * ( qb + ( qb >= 0.0 ? s : -s ) );
				roots[ found++ ] = k / qa;
				if( k != 0.0 )
					roots[ found++ ] = qc / k;
			}
		}
		std::sort( roots, roots + found );
		for( int i = 0; i < found; ++i )
			if( roots[ i ] > 0.0 && roots[ i ] < 1.0 )
				breaks[ count++ ] = roots[ i ];
		breaks[ count++ ] = 1.0;
	}
	double best = std::min( dot( d, d ), dot( p.p2 - q, p.p2 - q ) );
	for( int i = 0; i + 1 < count; ++i )
	{
		double lo = breaks[ i ], hi = breaks[ i + 1 ];
		if( !( g( lo ) < 0.0 && g( hi ) >= 0.0 ) )
			continue;
		for( int k = 0; k < 64; ++k )
		{
			const double m = 0.5 * ( lo + hi );
			( g( m ) < 0.0 ? lo : hi ) = m;
		}
		const Vec2 r = Evaluate( p, 0.5 * ( lo + hi ) ) - q;
		best         = std::min( best, dot( r, r ) );
	}
	return std::sqrt( best );
}

std::vector< Piece > CubicToQuads( Vec2 p0, Vec2 c1, Vec2 c2, Vec2 p3, double tolerance )
{
	std::vector< Piece > out;
	approximateCubic( p0, c1, c2, p3, tolerance, 0, out );
	return out;
}

double CubicDeviation( Vec2 p0, Vec2 c1, Vec2 c2, Vec2 p3, const std::vector< Piece >& pieces )
{
	double worst = 0.0;
	for( int i = 0; i <= 512; ++i )
	{
		const double t = i / 512.0, u = 1.0 - t;
		const Vec2 q   = p0 * ( u * u * u ) + c1 * ( 3.0 * u * u * t ) + c2 * ( 3.0 * u * t * t ) + p3 * ( t * t * t );
		double best    = std::numeric_limits< double >::max();
		for( const Piece& p : pieces )
			best = std::min( best, DistanceToPiece( p, q ) );
		worst = std::max( worst, best );
	}
	return worst;
}

int WindingAt( const std::vector< Piece >& pieces, Vec2 q, bool evenOdd )
{
	int winding = 0, count = 0;
	for( const Piece& p : pieces )
	{
		if( p.p0.y == p.p2.y )
			continue;
		const double lo = std::min( p.p0.y, p.p2.y ), hi = std::max( p.p0.y, p.p2.y );
		if( q.y < lo || q.y >= hi )
			continue;
		const double x = Evaluate( p, solveY( p, q.y ) ).x;
		if( x > q.x )
		{
			winding += p.p2.y > p.p0.y ? 1 : -1;
			++count;
		}
	}
	return evenOdd ? ( count & 1 ) : winding;
}

double BoundaryDistance( const std::vector< Piece >& pieces, Vec2 q )
{
	double best = std::numeric_limits< double >::max();
	for( const Piece& p : pieces )
		if( p.boundary )
			best = std::min( best, DistanceToPiece( p, q ) );
	return best;
}

GlyphOutline BuildOutline( const std::vector< Font::Vertex >& shape, int unitsPerEm, const OutlineOptions& options )
{
	const double em            = static_cast< double >( unitsPerEm );
	const double straightLimit = 1.0e-6 * em;

	//Contours of pieces, as the font draws them, every one closed.
	std::vector< std::vector< Piece > > contours;
	Vec2 start, pen;
	bool open  = false;
	auto close = [ & ]() {
		if( open && pen != start )
			contours.back().push_back( makeLine( pen, start ) );
		open = false;
	};
	auto addQuad = [ & ]( Vec2 a, Vec2 c, Vec2 b ) {
		//A quadratic back to where it started encloses nothing.
		if( a == b )
			return;
		if( nearlyStraight( a, c, b, straightLimit ) )
			contours.back().push_back( makeLine( a, b ) );
		else
		{
			Piece p;
			p.p0 = a;
			p.p1 = c;
			p.p2 = b;
			contours.back().push_back( p );
		}
	};
	for( const Font::Vertex& v : shape )
	{
		const Vec2 end = { static_cast< double >( v.x ), static_cast< double >( v.y ) };
		if( v.type == Font::Vertex::Move )
		{
			close();
			contours.emplace_back();
			start = pen = end;
			open        = true;
			continue;
		}
		if( !open )
			continue;
		switch( v.type )
		{
		case Font::Vertex::Line:
			if( end != pen )
				contours.back().push_back( makeLine( pen, end ) );
			break;
		case Font::Vertex::Quad:
			addQuad( pen, { static_cast< double >( v.cx ), static_cast< double >( v.cy ) }, end );
			break;
		case Font::Vertex::Cubic:
		{
			const Vec2 c1 = { static_cast< double >( v.cx ), static_cast< double >( v.cy ) };
			const Vec2 c2 = { static_cast< double >( v.dx ), static_cast< double >( v.dy ) };
			if( pen == end && c1 == end && c2 == end )
				break;
			for( const Piece& q : CubicToQuads( pen, c1, c2, end, kCubicTolerance * em ) )
				addQuad( q.p0, q.p1, q.p2 );
			break;
		}
		default: break;
		}
		pen = end;
	}
	close();

	//Monotonic in y, remembering which contour each piece came from and
	//where in it, so neighbours are not tested against each other.
	std::vector< Piece > pieces;
	std::vector< int > contourOf, first, last;
	for( size_t c = 0; c < contours.size(); ++c )
	{
		const int from = static_cast< int >( pieces.size() );
		for( const Piece& p : contours[ c ] )
			pushMonotonic( p, pieces );
		const int to = static_cast< int >( pieces.size() ) - 1;
		for( int i = from; i <= to; ++i )
		{
			contourOf.push_back( static_cast< int >( c ) );
			first.push_back( from );
			last.push_back( to );
		}
	}

	GlyphOutline out;
	if( pieces.empty() )
		return out;

	//Overlaps: split every piece where another crosses it.
	std::vector< std::vector< double > > splits( pieces.size() );
	if( options.resolveOverlaps )
	{
		const double size = 1.0e-6 * em;
		std::vector< Box > boxes;
		for( const Piece& p : pieces )
			boxes.push_back( controlBox( p ) );
		for( size_t i = 0; i < pieces.size(); ++i )
			for( size_t j = i + 1; j < pieces.size(); ++j )
			{
				if( contourOf[ i ] == contourOf[ j ] )
				{
					const bool next  = j == i + 1;
					const bool wraps = static_cast< int >( i ) == first[ i ] && static_cast< int >( j ) == last[ j ];
					if( next || wraps )
						continue;
				}
				if( !overlaps( boxes[ i ], boxes[ j ], size ) )
					continue;
				for( const Hit& h : crossings( pieces[ i ], pieces[ j ], size ) )
				{
					splits[ i ].push_back( h.t );
					splits[ j ].push_back( h.s );
				}
			}
	}
	std::vector< Piece > split;
	for( size_t i = 0; i < pieces.size(); ++i )
	{
		std::vector< double >& ts = splits[ i ];
		std::sort( ts.begin(), ts.end() );
		std::vector< double > cuts = { 0.0 };
		for( double t : ts )
			if( t > 1.0e-7 && t < 1.0 - 1.0e-7 && t - cuts.back() > 1.0e-7 )
				cuts.push_back( t );
		if( cuts.size() > 1 )
			out.crossings += static_cast< int >( cuts.size() ) - 1;
		cuts.push_back( 1.0 );
		Vec2 from = pieces[ i ].p0;
		for( size_t k = 0; k + 1 < cuts.size(); ++k )
		{
			const Vec2 to = k + 2 == cuts.size() ? pieces[ i ].p2 : Evaluate( pieces[ i ], cuts[ k + 1 ] );
			if( to != from )
				split.push_back( stretch( pieces[ i ], cuts[ k ], cuts[ k + 1 ], from, to ) );
			from = to;
		}
	}

	//Classify each piece by the winding just either side of its middle,
	//against every piece (the fill is the font's, overlaps and all).
	for( Piece& p : split )
	{
		p.boundary = true;
		if( !options.resolveOverlaps )
			continue;
		const Vec2 mid     = Evaluate( p, 0.5 );
		const Vec2 tangent = p.p2 - p.p0;//B'(1/2), halved
		const double span  = length( tangent );
		if( span == 0.0 )
			continue;
		const Vec2 normal  = { -tangent.y / span, tangent.x / span };
		const double reach = std::min( 1.0e-4 * em, 0.1 * span );
		const bool left    = WindingAt( split, mid + normal * reach ) != 0;
		const bool right   = WindingAt( split, mid - normal * reach ) != 0;
		p.boundary         = left != right;
		if( !p.boundary )
			++out.internal;
	}
	out.pieces = split;

	//The ink's bounds: each piece's ends, and its x-extremum (y is
	//monotonic, so the ends bound it in y).
	out.x0 = out.y0 = std::numeric_limits< double >::max();
	out.x1 = out.y1 = -std::numeric_limits< double >::max();
	auto include = [ & ]( Vec2 v ) {
		out.x0 = std::min( out.x0, v.x );
		out.y0 = std::min( out.y0, v.y );
		out.x1 = std::max( out.x1, v.x );
		out.y1 = std::max( out.y1, v.y );
	};
	for( const Piece& p : out.pieces )
	{
		include( p.p0 );
		include( p.p2 );
		const double denominator = p.p0.x - 2.0 * p.p1.x + p.p2.x;
		if( !p.line && denominator != 0.0 )
		{
			const double t = ( p.p0.x - p.p1.x ) / denominator;
			if( t > 0.0 && t < 1.0 )
				include( Evaluate( p, t ) );
		}
	}
	return out;
}

} // namespace knockout
