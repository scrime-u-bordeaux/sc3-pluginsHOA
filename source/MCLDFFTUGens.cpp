/*

FFT analysis and phase vocoder UGens for SuperCollider, by Dan Stowell.
(c) Dan Stowell 2006.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#include "SC_PlugIn.h"
#include "SCComplex.h"
#include "FFT_UGens.h"

//////////////////////////////////////////////////////////////////////////////////////////////////


struct FFTAnalyser_Unit : Unit
{
	float outval;
};

struct FFTAnalyser_OutOfPlace : FFTAnalyser_Unit
{
	int m_numbins;
	float *m_tempbuf;
};

struct FFTPercentile_Unit : FFTAnalyser_OutOfPlace 
{
};

struct FFTFlux_Unit : FFTAnalyser_OutOfPlace 
{
	float m_yesternorm;
	float m_yesterdc;
	float m_yesternyq;
	bool m_normalise;
};

struct FFTFlatnessSplitPercentile_Unit : FFTPercentile_Unit
{
	float outval2;
};

struct FFTSubbandPower : FFTAnalyser_Unit
{
	int m_numbands;
			int *m_cutoffs; // Will hold bin indices corresponding to frequencies
	float *m_outvals;
	bool m_cutoff_inited;
	bool m_inc_dc;
};


// for operation on one buffer
// just like PV_GET_BUF except it outputs unit->outval rather than -1 when FFT not triggered
#define FFTAnalyser_GET_BUF \
	float fbufnum = ZIN0(0); \
	if (fbufnum < 0.f) { ZOUT0(0) = unit->outval; return; } \
	ZOUT0(0) = fbufnum; \
	uint32 ibufnum = (uint32)fbufnum; \
	World *world = unit->mWorld; \
	if (ibufnum >= world->mNumSndBufs) ibufnum = 0; \
	SndBuf *buf = world->mSndBufs + ibufnum; \
	int numbins = buf->samples - 2 >> 1;

// Same as above; but with "output2" as well as "output"
#define FFTAnalyser_GET_BUF_TWOOUTS \
	float fbufnum = ZIN0(0); \
	if (fbufnum < 0.f) { ZOUT0(0) = unit->outval; ZOUT0(1) = unit->outval2; return; } \
	ZOUT0(0) = fbufnum; \
	uint32 ibufnum = (uint32)fbufnum; \
	World *world = unit->mWorld; \
	if (ibufnum >= world->mNumSndBufs) ibufnum = 0; \
	SndBuf *buf = world->mSndBufs + ibufnum; \
	int numbins = buf->samples - 2 >> 1;

// As above; but for operation on two input buffers
#define FFTAnalyser_GET_BUF2 \
	float fbufnum1 = ZIN0(0); \
	float fbufnum2 = ZIN0(1); \
	if (fbufnum1 < 0.f || fbufnum2 < 0.f) { ZOUT0(0) = unit->outval; return; } \
	uint32 ibufnum1 = (int)fbufnum1; \
	uint32 ibufnum2 = (int)fbufnum2; \
	World *world = unit->mWorld; \
	if (ibufnum1 >= world->mNumSndBufs) ibufnum1 = 0; \
	if (ibufnum2 >= world->mNumSndBufs) ibufnum2 = 0; \
	SndBuf *buf1 = world->mSndBufs + ibufnum1; \
	SndBuf *buf2 = world->mSndBufs + ibufnum2; \
	if (buf1->samples != buf2->samples) return; \
	int numbins = buf1->samples - 2 >> 1;

// Copied from FFT_UGens.cpp
#define MAKE_TEMP_BUF \
	if (!unit->m_tempbuf) { \
		unit->m_tempbuf = (float*)RTAlloc(unit->mWorld, buf->samples * sizeof(float)); \
		unit->m_numbins = numbins; \
	} else if (numbins != unit->m_numbins) return; 

//////////////////////////////////////////////////////////////////////////////////////////////////

extern "C"
{
	void load(InterfaceTable *inTable);

	void FFTPower_Ctor(FFTAnalyser_Unit *unit);
	void FFTPower_next(FFTAnalyser_Unit *unit, int inNumSamples);

	void FFTFlatness_Ctor(FFTAnalyser_Unit *unit);
	void FFTFlatness_next(FFTAnalyser_Unit *unit, int inNumSamples);

	void FFTPercentile_Ctor(FFTPercentile_Unit *unit);
	void FFTPercentile_next(FFTPercentile_Unit *unit, int inNumSamples);
	void FFTPercentile_Dtor(FFTPercentile_Unit *unit);

	void FFTFlux_Ctor(FFTFlux_Unit *unit);
	void FFTFlux_next(FFTFlux_Unit *unit, int inNumSamples);
	void FFTFlux_Dtor(FFTFlux_Unit *unit);
	void FFTFluxPos_Ctor(FFTFlux_Unit *unit);
	void FFTFluxPos_next(FFTFlux_Unit *unit, int inNumSamples);
	void FFTFluxPos_Dtor(FFTFlux_Unit *unit);

	void FFTFlatnessSplitPercentile_Ctor(FFTFlatnessSplitPercentile_Unit *unit);
	void FFTFlatnessSplitPercentile_next(FFTFlatnessSplitPercentile_Unit *unit, int inNumSamples);
	void FFTFlatnessSplitPercentile_Dtor(FFTFlatnessSplitPercentile_Unit *unit);

	void FFTDiffMags_Ctor(FFTAnalyser_Unit *unit);
	void FFTDiffMags_next(FFTAnalyser_Unit *unit, int inNumSamples);

//	void PV_DiffAndToDC_Ctor(PV_Unit *unit);
//	void PV_DiffAndToDC_next(PV_Unit *unit, int inNumSamples);

	void PV_DiffMags_Ctor(PV_Unit *unit);
	void PV_DiffMags_next(PV_Unit *unit, int inNumSamples);

	void FFTSubbandPower_Ctor(FFTSubbandPower *unit);
	void FFTSubbandPower_next(FFTSubbandPower *unit, int inNumSamples);
	void FFTSubbandPower_Dtor(FFTSubbandPower *unit);

}

SCPolarBuf* ToPolarApx(SndBuf *buf)
{
	if (buf->coord == coord_Complex) {
		SCComplexBuf* p = (SCComplexBuf*)buf->data;
		int numbins = buf->samples - 2 >> 1;
		for (int i=0; i<numbins; ++i) {
			p->bin[i].ToPolarApxInPlace();
		}
		buf->coord = coord_Polar;
	}
	return (SCPolarBuf*)buf->data;
}

SCComplexBuf* ToComplexApx(SndBuf *buf)
{
	if (buf->coord == coord_Polar) {
		SCPolarBuf* p = (SCPolarBuf*)buf->data;
		int numbins = buf->samples - 2 >> 1;
		for (int i=0; i<numbins; ++i) {
			p->bin[i].ToComplexApxInPlace();
		}
		buf->coord = coord_Complex;
	}
	return (SCComplexBuf*)buf->data;
}

InterfaceTable *ft;

void init_SCComplex(InterfaceTable *inTable);

//////////////////////////////////////////////////////////////////////////////////////////////////

void FFTPower_next(FFTAnalyser_Unit *unit, int inNumSamples)
{
	FFTAnalyser_GET_BUF

	SCComplexBuf *p = ToComplexApx(buf);
	
	float total = sc_abs(p->dc) + sc_abs(p->nyq);
	
	for (int i=0; i<numbins; ++i) {
		float rabs = (p->bin[i].real);
		float iabs = (p->bin[i].imag);
		total += sqrt((rabs*rabs) + (iabs*iabs));
	}

	// Store the val for output in future calls
	unit->outval = total / (numbins + 2.);

	ZOUT0(0) = unit->outval;
}

void FFTPower_Ctor(FFTAnalyser_Unit *unit)
{
	SETCALC(FFTPower_next);
	ZOUT0(0) = unit->outval = 0.;
}

//////////////////////////////////////////////////////////////////////////////////////////////////

void FFTSubbandPower_next(FFTSubbandPower *unit, int inNumSamples)
{
	int numbands = unit->m_numbands;
	int numcutoffs = numbands - 1;
	
	
	// Multi-output equiv of FFTAnalyser_GET_BUF
	float fbufnum = ZIN0(0);
	if (fbufnum < 0.f) {
		for(int i=0; i<numbands; i++){
			ZOUT0(i) = unit->m_outvals[i];
		}
		return;
	}
	uint32 ibufnum = (uint32)fbufnum;
	World *world = unit->mWorld;
	if (ibufnum >= world->mNumSndBufs) ibufnum = 0;
	SndBuf *buf = world->mSndBufs + ibufnum;
	int numbins = buf->samples - 2 >> 1;
	// End: Multi-output equiv of FFTAnalyser_GET_BUF
	
	// Now we create the integer lookup list, if it doesn't already exist
	int * cutoffs = unit->m_cutoffs;
	if(!unit->m_cutoff_inited){
		
		float srate = world->mFullRate.mSampleRate;
		for(int i=0; i < numcutoffs; i++) {
			cutoffs[i] = (int)(buf->samples * ZIN0(3 + i) / srate);
			//Print("Allocated bin cutoff #%d, at bin %d\n", i, cutoffs[i]);
		}
		
		unit->m_cutoff_inited = true;
	}
	
	SCComplexBuf *p = ToComplexApx(buf);

	// Now we can actually calculate the bandwise subtotals
	float total = unit->m_inc_dc ? sc_abs(p->dc) : 0;
	int binaddcount = unit->m_inc_dc ? 1 : 0; // Counts how many bins contributed to the current band (1 because of the DC value)
	int curband = 0;
	float * outvals = unit->m_outvals;
	
	for (int i=0; i<numbins; ++i) {
		if(i == cutoffs[curband]){
			outvals[curband] = total / binaddcount;
			curband++;
			total = 0.f;
			binaddcount = 0;
		}
		
		float rabs = (p->bin[i].real);
		float iabs = (p->bin[i].imag);
		total += sqrt((rabs*rabs) + (iabs*iabs));
		binaddcount++;
	}

	// Remember to output the very last (highest) band
	total += sc_abs(p->nyq);
	outvals[curband] = total / (binaddcount + 1); // Plus one because of the nyq value

	// Now we can output the vals
	for(int i=0; i<numbands; i++) {
		ZOUT0(i) = outvals[i];
	}
}

void FFTSubbandPower_Ctor(FFTSubbandPower *unit)
{
	SETCALC(FFTSubbandPower_next);
	
	// ZIN0(1) tells us how many cutoffs we're looking for
	int numcutoffs = (int)ZIN0(1);
	int numbands = numcutoffs+1;
	
	float * outvals = (float*)RTAlloc(unit->mWorld, numbands * sizeof(float));
	for(int i=0; i<numbands; i++) {
		outvals[i] = 0.f;
	}
	unit->m_outvals = outvals;
	
	unit->m_cutoffs = (int*)RTAlloc(unit->mWorld, 
			numcutoffs * sizeof(int)
		);
	
	unit->m_cutoff_inited = false;

	unit->m_inc_dc = ZIN0(2) > 0.f;
	
	unit->m_numbands = numbands;
	ZOUT0(0) = unit->outval = 0.;
}

void FFTSubbandPower_Dtor(FFTSubbandPower *unit)
{
	RTFree(unit->mWorld, unit->m_cutoffs);
	RTFree(unit->mWorld, unit->m_outvals);
}

//////////////////////////////////////////////////////////////////////////////////////////////////

void FFTFlatness_next(FFTAnalyser_Unit *unit, int inNumSamples)
{
	FFTAnalyser_GET_BUF

	SCComplexBuf *p = ToComplexApx(buf);
	
	// Spectral Flatness Measure is geometric mean divided by arithmetic mean.
	//
	// In order to calculate geom mean without hitting the precision limit, 
	//  we use the trick of converting to log, taking the average, then converting back from log.
	double geommean = log(sc_abs(p->dc)) + log(sc_abs(p->nyq));
	double mean     = sc_abs(p->dc)      + sc_abs(p->nyq);
	
	for (int i=0; i<numbins; ++i) {
		float rabs = (p->bin[i].real);
		float iabs = (p->bin[i].imag);
		float amp = sqrt((rabs*rabs) + (iabs*iabs));
		geommean += log(amp);
		mean += amp;
	}

	double oneovern = 1/(numbins + 2.);
	geommean = exp(geommean * oneovern); // Average and then convert back to linear
	mean *= oneovern;

	// Store the val for output in future calls
	unit->outval = geommean / mean;

	ZOUT0(0) = unit->outval;
}

void FFTFlatness_Ctor(FFTAnalyser_Unit *unit)
{
	SETCALC(FFTFlatness_next);
	ZOUT0(0) = unit->outval = 0.;
}

////////////////////////////////////////////////////////////////////////////////////


void FFTPercentile_next(FFTPercentile_Unit *unit, int inNumSamples)
{
	FFTAnalyser_GET_BUF
	MAKE_TEMP_BUF

	// Percentile value as a fraction. eg: 0.5 == 50-percentile (median).
	float fraction = ZIN0(1);

	// The magnitudes in *p will be converted to cumulative sum values and stored in *q temporarily
	SCComplexBuf *p = ToComplexApx(buf);
	SCComplexBuf *q = (SCComplexBuf*)unit->m_tempbuf;
	
	float cumul = sc_abs(p->dc);
	
	for (int i=0; i<numbins; ++i) {
		float real = p->bin[i].real;
		float imag = p->bin[i].imag;
		cumul += sqrt(real*real + imag*imag);
		
		// A convenient place to store the mag values...
		q->bin[i].real = cumul;
	}
	
	cumul += sc_abs(p->nyq);
	
	float target = cumul * fraction; // The target cumul value, stored in the "real" slots

	float bestposition = 0; // May be linear-interpolated between bins, but not implemented yet
	           // NB If nothing beats the target (e.g. if fraction is -1) zero Hz is returned
	float nyqfreq = ((float)unit->mWorld->mSampleRate) * 0.5f;
	for(int i=0; i<numbins; i++) {
		//Print("Testing %g, at position %i", q->bin[i].real, i);
		if(q->bin[i].real >= target){
			bestposition = (nyqfreq * (float)(i+1)) / (float)(numbins+2);
			//Print("Target %g beaten by %g (at position %i), equating to freq %g\n", 
			//				target, p->bin[i].real, i, bestposition);
			break;
		}
	}
/* THIS COUNTDOWN METHOD DEPRECATED IN FAVOUR OF COUNT-UP, for various reasons.
	for(int i=numbins-1; i>-1; i--) {
		//Print("Testing %g, at position %i", q->bin[i].real, i);
		if(q->bin[i].real <= target){
			bestposition = (nyqfreq * (float)i) / (float)numbins;
			//Print("Target %g beaten by %g (at position %i), equating to freq %g\n", 
			//				target, p->bin[i].real, i, bestposition);
			break;
		}
	}
*/

	// Store the val for output in future calls
	unit->outval = bestposition;

	ZOUT0(0) = unit->outval;
}

void FFTPercentile_Ctor(FFTPercentile_Unit *unit)
{
	SETCALC(FFTPercentile_next);

//	unit->m_subtotals = (float*)RTAlloc(unit->mWorld, N * sizeof(float));

	ZOUT0(0) = unit->outval = 0.;
	unit->m_tempbuf = 0;
}

void FFTPercentile_Dtor(FFTPercentile_Unit *unit)
{
	RTFree(unit->mWorld, unit->m_tempbuf);
}

////////////////////////////////////////////////////////////////////////////////////

void FFTFlux_Ctor(FFTFlux_Unit *unit)
{
	SETCALC(FFTFlux_next);

	unit->m_tempbuf = 0;

	unit->m_yesternorm = 1.0f;
	unit->m_yesterdc = 0.0f;
	unit->m_yesternyq = 0.0f;
	
	unit->m_normalise = ZIN0(1) > 0.f; // Whether we want to normalise or not

	unit->outval = 0.f;
	ZOUT0(0) = 0.f;
}

void FFTFlux_next(FFTFlux_Unit *unit, int inNumSamples)
{
	FFTAnalyser_GET_BUF

	// Modified form of MAKE TEMP BUF, here used for storing last frame's magnitudes:
	if (!unit->m_tempbuf) {
		unit->m_tempbuf = (float*)RTAlloc(unit->mWorld, numbins * sizeof(float));
		unit->m_numbins = numbins;
		
		// Must also ensure the yester is zero'ed
		// because it will be compared before being filled in FFTFlux calculation
		memset(unit->m_tempbuf, 0, numbins * sizeof(float));
	} else if (numbins != unit->m_numbins) return;
	
	SCPolarBuf *p = ToPolarApx(buf); // Current frame
	float* yestermags = unit->m_tempbuf; // This is an array storing the yester magnitudes
	
	float yesternorm = unit->m_yesternorm; // Should have been calculated on prev cycle
	
	float currnorm;
	if(unit->m_normalise){
		// First iteration is to find the sum of magnitudes (to find the normalisation factor):
		currnorm = (p->dc * p->dc) + (p->nyq * p->nyq);
		for (int i=0; i<numbins; ++i) {
			currnorm += p->bin[i].mag * p->bin[i].mag;
		}
		// The normalisation factor is 1-over-sum
		if(currnorm != 0.0f) {
			currnorm = 1.0f / currnorm;
		}
	} else {
		currnorm = 1.f;
	}
	
	// This iteration is the meat of the algorithm. Compare current (normed) bins against prev.
	float onebindiff  = sc_abs(p->dc  * currnorm) - sc_abs(unit->m_yesterdc  * yesternorm);
	float fluxsquared = (onebindiff * onebindiff);
	onebindiff        = sc_abs(p->nyq * currnorm) - sc_abs(unit->m_yesternyq * yesternorm);
	fluxsquared      += (onebindiff * onebindiff);
	// Now the bins
	for (int i=0; i<numbins; ++i) {
		// Sum the squared difference of normalised mags onto the cumulative value
		onebindiff = (p->bin[i].mag * currnorm) - (yestermags[i] * yesternorm);
		fluxsquared += (onebindiff * onebindiff);
		// Overwrite yestermag values with current values, so they're available next time
		yestermags[i] = p->bin[i].mag;
	}
	
	// Store the just-calc'ed norm as yesternorm
	unit->m_yesternorm = currnorm;
	unit->m_yesterdc = p->dc;
	unit->m_yesternyq = p->nyq;
	
	// Store the val for output in future calls
	unit->outval = sqrt(fluxsquared);

	ZOUT0(0) = unit->outval;

}
void FFTFlux_Dtor(FFTFlux_Unit *unit)
{
	RTFree(unit->mWorld, unit->m_tempbuf);
}


////////////////////////////////////////////////////////////////////////////////////

void FFTFluxPos_Ctor(FFTFlux_Unit *unit)
{
	SETCALC(FFTFluxPos_next);

	unit->m_tempbuf = 0;

	unit->m_yesternorm = 1.0f;
	unit->m_yesterdc = 0.0f;
	unit->m_yesternyq = 0.0f;

	unit->outval = 0.f;
	ZOUT0(0) = 0.f;
}

void FFTFluxPos_next(FFTFlux_Unit *unit, int inNumSamples)
{
	FFTAnalyser_GET_BUF

	// Modified form of MAKE TEMP BUF, here used for storing last frame's magnitudes:
	if (!unit->m_tempbuf) {
		unit->m_tempbuf = (float*)RTAlloc(unit->mWorld, numbins * sizeof(float));
		unit->m_numbins = numbins;
		
		// Must also ensure the yester is zero'ed
		// because it will be compared before being filled in FFTFlux calculation
		memset(unit->m_tempbuf, 0, numbins * sizeof(float));
	} else if (numbins != unit->m_numbins) return;
	
	SCPolarBuf *p = ToPolarApx(buf); // Current frame
	float* yestermags = unit->m_tempbuf; // This is an array storing the yester magnitudes
	
	float yesternorm = unit->m_yesternorm; // Should have been calculated on prev cycle

	float currnorm;
	if(unit->m_normalise){
		// First iteration is to find the sum of magnitudes (to find the normalisation factor):
		currnorm = (p->dc * p->dc) + (p->nyq * p->nyq);
		for (int i=0; i<numbins; ++i) {
			currnorm += p->bin[i].mag * p->bin[i].mag;
		}
		// The normalisation factor is 1-over-sum
		if(currnorm != 0.0f) {
			currnorm = 1.0f / currnorm;
		}
	} else {
		currnorm = 1.f;
	}

	
	// This iteration is the meat of the algorithm. Compare current (normed) bins against prev.
	float onebindiff  = sc_abs(p->dc  * currnorm) - sc_abs(unit->m_yesterdc  * yesternorm);
	float fluxsquared = 0.f;
	if(onebindiff > 0.f)
		fluxsquared += (onebindiff * onebindiff);
	onebindiff        = sc_abs(p->nyq * currnorm) - sc_abs(unit->m_yesternyq * yesternorm);
	if(onebindiff > 0.f)
		fluxsquared += (onebindiff * onebindiff);
	// Now the bins
	for (int i=0; i<numbins; ++i) {
		// Sum the squared difference of normalised mags onto the cumulative value
		onebindiff = (p->bin[i].mag * currnorm) - (yestermags[i] * yesternorm);

		////////// THIS IS WHERE FFTFluxPos DIFFERS FROM FFTFlux - THE SIMPLE ADDITION OF AN "IF": //////////
		if(onebindiff > 0.f) // The IF only applies to the next line - formatting is a bit weird to keep in line with the other func

		fluxsquared += (onebindiff * onebindiff);
		// Overwrite yestermag values with current values, so they're available next time
		yestermags[i] = p->bin[i].mag;
	}
	
	// Store the just-calc'ed norm as yesternorm
	unit->m_yesternorm = currnorm;
	unit->m_yesterdc = p->dc;
	unit->m_yesternyq = p->nyq;
	
	// Store the val for output in future calls
	unit->outval = sqrt(fluxsquared);

	ZOUT0(0) = unit->outval;

}
void FFTFluxPos_Dtor(FFTFlux_Unit *unit)
{
	RTFree(unit->mWorld, unit->m_tempbuf);
}

////////////////////////////////////////////////////////////////////////////////////


void FFTFlatnessSplitPercentile_next(FFTFlatnessSplitPercentile_Unit *unit, int inNumSamples)
{
	FFTAnalyser_GET_BUF
	MAKE_TEMP_BUF

	// Percentile value as a fraction. eg: 0.5 == 50-percentile (median).
	float fraction = ZIN0(1);

	// The magnitudes in *p will be converted to cumulative sum values and stored in *q temporarily
	SCComplexBuf *p = ToComplexApx(buf);
	SCComplexBuf *q = (SCComplexBuf*)unit->m_tempbuf;
	
	// Spectral Flatness Measure is geometric mean divided by arithmetic mean.
	//
	// In order to calculate geom mean without hitting the precision limit, 
	//  we use the trick of converting to log, taking the average, then converting back from log.
	double geommeanupper = log(sc_abs(p->nyq));
	double meanupper     = sc_abs(p->nyq);
	double geommeanlower = log(sc_abs(p->dc));
	double meanlower     = sc_abs(p->dc);

	float cumul = sc_abs(p->dc);
	for (int i=0; i<numbins; ++i) {
		float real = p->bin[i].real;
		float imag = p->bin[i].imag;
		float amp = sqrt(real*real + imag*imag);
		cumul += amp;
		
		// A convenient place to store the mag values...
		// NOTE: The values stored here are NOT real and imag pairs.
		q->bin[i].real = cumul;
		q->bin[i].imag = amp;
	}
	cumul += sc_abs(p->nyq);
	
	float target = cumul * fraction; // The target cumul value, stored in the "real" slots

	int numupper = -1;
	int numlower = -1;
	for(int i=numbins-1; i>-1; i--) {
	
		float amp = q->bin[i].imag; // This is where I stored the amp earlier.
		
		if(numupper == -1) {
			//Print("Testing %g, at position %i", q->bin[i].real, i);
			if(q->bin[i].real <= target){ // We are transitioning from upper to lower region
				//bestposition = (nyqfreq * (float)i) / (float)numbins;
				//Print("Target %g beaten by %g (at position %i), equating to freq %g\n", 
				//				target, p->bin[i].real, i, bestposition);
				geommeanlower += log(amp);
				meanlower += amp;
				numupper = numbins - i; // inc nyq, therefore skip the "minus one"
				numlower = i + 2; // inc DC, therefore "plus two" rather than "plus one"
			} else { // We're still in the upper portion of the spectrum
				geommeanupper += log(amp);
				meanupper += amp;
			}
		} else { // We're in the lower portion of the spectrum
			geommeanlower += log(amp);
			meanlower += amp;
		}
	} // End of iteration backwards over the bins
	
	if(numupper == -1) { // Should be very unlikely, but may happen (e.g. if fraction==-1)
		numupper = numbins + 1; // All, plus nyquist
		numlower = 1; // Just the DC
	}
	
	geommeanupper = exp(geommeanupper / numupper); // Average and then convert back to linear
	meanupper /= numupper;
	geommeanlower = exp(geommeanlower / numlower); // Average and then convert back to linear
	meanlower /= numlower;

	// Store the val for output in future calls
	unit->outval  = geommeanlower / meanlower;
	unit->outval2 = geommeanupper / meanupper;

	ZOUT0(0) = unit->outval;
	ZOUT0(1) = unit->outval2;
}

void FFTFlatnessSplitPercentile_Ctor(FFTFlatnessSplitPercentile_Unit *unit)
{
	SETCALC(FFTFlatnessSplitPercentile_next);

//	unit->m_subtotals = (float*)RTAlloc(unit->mWorld, N * sizeof(float));

	ZOUT0(0) = unit->outval = 0.;
	ZOUT0(1) = unit->outval2 = 0.;
	unit->m_tempbuf = 0;
}

void FFTFlatnessSplitPercentile_Dtor(FFTFlatnessSplitPercentile_Unit *unit)
{
	RTFree(unit->mWorld, unit->m_tempbuf);
}

////////////////////////////////////////////////////////////////////////////////////

void FFTDiffMags_next(FFTAnalyser_Unit *unit, int inNumSamples)
{

	FFTAnalyser_GET_BUF2
	
	SCComplexBuf *p = ToComplexApx(buf1);
	SCComplexBuf *q = ToComplexApx(buf2);
	
	// First the DC and nyquist.
	float diffsum = sc_abs(p->dc - q->dc) + sc_abs(p->nyq - q->nyq);

	for (int i=0; i<numbins; ++i) {
		float rdiff = p->bin[i].real - q->bin[i].real;
		float idiff = p->bin[i].imag - q->bin[i].imag;

		diffsum += sqrt(rdiff*rdiff + idiff*idiff);
	}

	// Store the val for output in future calls
	unit->outval = diffsum / (numbins + 2);

    //Print("FFTDiffMags_next: output is %g\n", unit->outval);

	ZOUT0(0) = unit->outval;
}

void FFTDiffMags_Ctor(FFTAnalyser_Unit *unit)
{
	SETCALC(FFTDiffMags_next);
	ZOUT0(0) = ZIN0(0);
}

////////////////////////////////////////////////////////////////////////////////////


void PV_DiffMags_next(PV_Unit *unit, int inNumSamples)
{
	PV_GET_BUF2
	
	SCPolarBuf *p = ToPolarApx(buf1);
	SCPolarBuf *q = ToPolarApx(buf2);
	
	// First the DC and nyquist
	p->dc  = p->dc - q->dc;
	p->nyq = p->nyq - q->nyq;

	for (int i=0; i<numbins; ++i) {
		p->bin[i].mag -= q->bin[i].mag;
	}
}

void PV_DiffMags_Ctor(PV_Unit *unit)
{
	SETCALC(PV_DiffMags_next);
	ZOUT0(0) = ZIN0(0);
}


////////////////////////////////////////////////////////////////////////////////////////////////////////


void load(InterfaceTable *inTable)
{
	ft= inTable;

	init_SCComplex(inTable);

	(*ft->fDefineUnit)("FFTPower", sizeof(FFTAnalyser_Unit), (UnitCtorFunc)&FFTPower_Ctor, 0, 0);
	(*ft->fDefineUnit)("FFTFlatness", sizeof(FFTAnalyser_Unit), (UnitCtorFunc)&FFTFlatness_Ctor, 0, 0);
	(*ft->fDefineUnit)("FFTPercentile", sizeof(FFTPercentile_Unit), (UnitCtorFunc)&FFTPercentile_Ctor, (UnitDtorFunc)&FFTPercentile_Dtor, 0);
	(*ft->fDefineUnit)("FFTFlux", sizeof(FFTFlux_Unit), (UnitCtorFunc)&FFTFlux_Ctor, (UnitDtorFunc)&FFTFlux_Dtor, 0);
	(*ft->fDefineUnit)("FFTFluxPos", sizeof(FFTFlux_Unit), (UnitCtorFunc)&FFTFluxPos_Ctor, (UnitDtorFunc)&FFTFluxPos_Dtor, 0);
	(*ft->fDefineUnit)("FFTFlatnessSplitPercentile", sizeof(FFTFlatnessSplitPercentile_Unit), (UnitCtorFunc)&FFTFlatnessSplitPercentile_Ctor, (UnitDtorFunc)&FFTFlatnessSplitPercentile_Dtor, 0);
	(*ft->fDefineUnit)("FFTDiffMags", sizeof(FFTAnalyser_Unit), (UnitCtorFunc)&FFTDiffMags_Ctor, 0, 0);
	(*ft->fDefineUnit)("PV_DiffMags", sizeof(PV_Unit), (UnitCtorFunc)&PV_DiffMags_Ctor, 0, 0);
	DefineDtorUnit(FFTSubbandPower);

}