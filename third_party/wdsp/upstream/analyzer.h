/*  analyzer.h

This file is part of a program that implements a Spectrum Analyzer
used in conjunction with software-defined-radio hardware.

Copyright (C) 2012, 2013, 2014, 2016, 2023, 2025 Warren Pratt, NR0V

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

The author can be reached by email at  

warren@wpratt.com

*/
#ifndef _analyzer_h
#define _analyzer_h
#include "comm.h"

// AetherSDR patch 25: most LO changes one FFT window can straddle.
#define dMAX_RETUNE_EV 64

typedef struct _dp
{
	int max_size;											// maximum fft size to be used
	int max_num_fft;										// maximum number of LO positions per sub-span to be used
	int max_stitch;											// maximum number of sub-spans to be concatenated
	int plan_estimate;										// AetherSDR patch 24: plan this display's FFTs with FFTW_ESTIMATE (0 = upstream's FFTW_PATIENT)
															//		NOTE:  max_size, max_num_fft, and max_stitch MUST BE <= THE
															//		CORRESPONDING VALUES IN <analyzer.h>!!
	int num_fft;											// current number of ffts in use
	int num_pixout;											// current number of detector/averages/pixel value outputs
	int size;												// current size of fft input sample vector
	int out_size;											// current size of fft output vector
	int window_type;										// type of the window function to be applied
	int overlap;											// number of samples re-used per fft, range 0 to size-1
	int flip[dMAX_NUM_FFT];									// 0 for low-side LO => do NOT flip; 1 for high-side LO => FLIP
	int clip;												// number of bins to clip off on EACH end of the sub-span fft
															//		ASSUMES size/2 IS AN EVEN NUMBER!!!
	double fsclipL;											// number of intervals to clip off the lower end of the TOTAL SPAN
	double fsclipH;											// number of intervals to clip off the upper end of the TOTAL SPAN
	int fscL;												// fsclipL modulo (out_size - 2 * clip)
	int fscH;												// fsclipH modulo (out_size - 2 * clip)
	int begin_ss;											// number of first sub-span that is NOT completely clipped off
	int end_ss;												// number of last sub-span that is NOT completely clipped off
	int ss_bins[dMAX_STITCH];								// number of bins delivered by eliminate()/Celiminate in each sub-span
	volatile LONG input_busy[dMAX_STITCH][dMAX_NUM_FFT];
	int num_pixels;											// number of pixels requested
	int num_stitch;											// number of results to be stitched together to generate the pixel frame
	unsigned long long stitch_flag;
	int spec_flag[dMAX_STITCH];								// flags showing if all ffts for a sub-span are done so elimination can proceed
	double pix_per_bin;										// number of pixels per fft bin, note that this is fractional, not integral
	double det_offset;										// offset needed in detector
	double bin_per_pix;										// number of fft bins per pixel, this is fractional and != 1.0/pix_per_bin
	double scale;											// output amplitude scale factor
	double PiAlpha;											// parameter for Kaiser window function

	int cal_set;											// specifies which set of calibration data to use
	double f_min;											// frequency at first pixel (for calibration)
	double f_max;											// frequency at last pixel (for calibration)
	int cal_changed;										// flag to indicate that the calibration data has changed

	double *window;											// pointer to buffer to hold window coefficients	
	double *result[dMAX_STITCH];							// pointers to buffer to hold elimination results for each sub-span
	dOUTREAL *pixels[dMAX_PIXOUTS][dNUM_PIXEL_BUFFS];		// pointers pixel output buffers
	double *t_pixels[dMAX_PIXOUTS];							// pointer to temporary pixel buffer									//pointer to temporary pixel buffer for non-averaged data
	int w_pix_buff[dMAX_PIXOUTS];							// number of pixel buffer owned by writing process
	int r_pix_buff[dMAX_PIXOUTS];							// number of pixel buffer owned by reading process
	int last_pix_buff[dMAX_PIXOUTS];						// number of the last pixel buffer written
	volatile LONG pb_ready[dMAX_PIXOUTS][dNUM_PIXEL_BUFFS];	// if value is 0, this data has already been read; 1 = fresh data to read
	int num_average[dMAX_PIXOUTS];							// number of spans to average to create the pixels
	int avail_frames[dMAX_PIXOUTS];							// number of pixel frames currently available to average
	int av_in_idx[dMAX_PIXOUTS];							// input index in averaging pixel buffer ring
	int av_out_idx[dMAX_PIXOUTS];							// output index in averaging pixel buffer ring
	double *av_sum[dMAX_PIXOUTS];							// pointer to sum buffer for averaging
	double *av_buff[dMAX_PIXOUTS][dMAX_AVERAGE];			// pointers to ring of buffers to hold pixel frames for averaging
	double *pre_av_out;
	int av_mode[dMAX_PIXOUTS];
	double av_backmult[dMAX_PIXOUTS];						// back multiplier for weighted averaging
	double *cd;												// pointer to amplitude calibration buffer
	int n_freqs[dMAX_CAL_SETS];								// number of frequencies in each calibration set
	double *freqs[dMAX_CAL_SETS];							// pointers to vectors of calibration frequencies
	double (*ac3[dMAX_CAL_SETS][dMAX_M]);					// pointers to amplitude interpolant coefficients
	double (*ac2[dMAX_CAL_SETS][dMAX_M]);
	double (*ac1[dMAX_CAL_SETS][dMAX_M]);
	double (*ac0[dMAX_CAL_SETS][dMAX_M]);

	fftw_plan plan[dMAX_STITCH][dMAX_NUM_FFT];				// fftw plans
	fftw_plan Cplan[dMAX_STITCH][dMAX_NUM_FFT];
	double *fft_in[dMAX_STITCH][dMAX_NUM_FFT];				// pointers to fftw real input vectors
	fftw_complex *Cfft_in[dMAX_STITCH][dMAX_NUM_FFT];		// pointers to fftw complex input vectors
	fftw_complex *fft_out[dMAX_STITCH][dMAX_NUM_FFT];		// pointers to fftw complex output vectors
	volatile LONG *pnum_threads;							// pointer to current number of active worker threads
	volatile LONG stop;										// when set, fft threads will be returned to the pool (AetherSDR patch 17: atomic)
	volatile LONG end_dispatcher;							// set this flag to one to destroy the dispatcher thread (AetherSDR patch 17: atomic)
	volatile int dispatcher;								// one if the dispatcher thread is alive & active
	int ss;													// sub-span being processed
	int LO;													// LO (within current sub-span) being processed 
	int flag;
	int have_samples[dMAX_STITCH][dMAX_NUM_FFT];			// number of unused samples remaining in a buffer
	int type;												// 0 for REAL, 1 for COMPLEX
	int incr;												// size - overlap
	int buff_size;											// amount of data to be stored each time an input buffer is opened and closed = JanusAudio/BlockSize
	dINREAL* I_samples[dMAX_STITCH][dMAX_NUM_FFT];			// pointers to current input position in I/Q buffers
	dINREAL* Q_samples[dMAX_STITCH][dMAX_NUM_FFT];
	int bsize;												// size of I_samples[][] and Q_samples[][] (number of samples they hold)
	int IQout_index[dMAX_STITCH][dMAX_NUM_FFT];				// current output index for I_samples[ss][LO] and Q_samples[ss][LO]
	int IQO_idx[dMAX_STITCH][dMAX_NUM_FFT];
	int IQin_index[dMAX_STITCH][dMAX_NUM_FFT];				// current input index for I_samples[ss][LO] and Q_samples[ss][LO]
	volatile LONG buff_ready[dMAX_STITCH][dMAX_NUM_FFT];	// 1 if buffer ready to read; 0 if needs to be filled
	int max_writeahead;										// max allowed input samples ahead of where reading output samples

	volatile LONG snap[dMAX_STITCH][dMAX_NUM_FFT];			// set to 1 to allow a snap of raw spectrum data
	HANDLE hSnapEvent[dMAX_STITCH][dMAX_NUM_FFT];			// mutex handles; mutexes will be used to signal a snap is complete
	double *snap_buff[dMAX_STITCH][dMAX_NUM_FFT];			// pointers to buffers for the snap

	CRITICAL_SECTION PB_ControlsSection[dMAX_PIXOUTS];
	CRITICAL_SECTION SetAnalyzerSection;
	CRITICAL_SECTION BufferControlSection[dMAX_STITCH][dMAX_NUM_FFT];
	CRITICAL_SECTION StitchSection;
	CRITICAL_SECTION EliminateSection[dMAX_STITCH];
	CRITICAL_SECTION ResampleSection;

	int det_type[dMAX_PIXOUTS];								// detector type
	double inv_coherent_gain;
	double inherent_power_gain;
	double inv_enb;
	double norm_oneHz;										// dB factor to normalize to one Hz bandwidth
	int sample_rate;										// sample rate; used for normalization calculations
	int normalize[dMAX_PIXOUTS];

	// BEGIN CODE TO GET MAX FFT_BIN WITHIN A FREQUENCY RANGE
	int dmb_run;
	int dmb_disp;
	int dmb_ss;
	int dmb_LO;
	double dmb_rate;
	double dmb_fLow;
	double dmb_fHigh;
	double dmb_tau;
	int dmb_frame_rate;
	int dmb_begin0;
	int dmb_end0;
	int dmb_begin1;
	int dmb_end1;
	double dmb_decay;
	double dmb_max_dB;
	CRITICAL_SECTION cs_dmb;
	// END CODE TO GET MAX FFT_BIN WITHIN A FREQUENCY RANGE

	// AetherSDR patch 25: retune rotation (brick3-angelia, after the smooth
	// pan / retune of the Jull1ano Thetis fork's analyzer.c). Every LO change
	// is marked in the input sample stream; when an FFT window holds samples
	// taken before the last change, those are frequency-rotated onto the
	// newest LO (phase continuous, as the radio's NCO), so a frame stays
	// coherent while the LO moves under a drag. Each pixel frame is tagged
	// with the LO it is computed for and the part of the band its samples
	// really cover. Complex input, one sub-span, one LO position only.
	int rt_enable;											// rotation on (SetAnalyzerRetune)
	double rt_fs;											// the REAL input sample rate, Hz (sample_rate may be repurposed)
	double rt_lat_ms;										// from SetAnalyzerLo() to the first sample taken at that LO
	int rt_lo_valid;										// an LO has been given
	long long rt_in_count[dMAX_STITCH][dMAX_NUM_FFT];		// input samples received so far
	long long rt_win_start[dMAX_STITCH][dMAX_NUM_FFT];		// absolute index of the first sample of the last dispatched window
	int rt_ev_count;										// pending LO changes, oldest first
	long long rt_ev_idx[dMAX_RETUNE_EV];					// absolute input sample index at which that LO starts
	double rt_ev_lo[dMAX_RETUNE_EV];						// that LO, Hz
	double rt_base_lo;										// LO of the samples before rt_ev_idx[0], Hz
	double rt_fi_lo[dMAX_STITCH][dMAX_NUM_FFT];				// the window just filled: its LO, Hz (0 = unknown)
	double rt_fi_vlo[dMAX_STITCH][dMAX_NUM_FFT];			// ...and its valid part, Hz relative to that LO
	double rt_fi_vhi[dMAX_STITCH][dMAX_NUM_FFT];
	double rt_pix_lo[dMAX_PIXOUTS][dNUM_PIXEL_BUFFS];		// the same, per pixel buffer
	double rt_pix_vlo[dMAX_PIXOUTS][dNUM_PIXEL_BUFFS];
	double rt_pix_vhi[dMAX_PIXOUTS][dNUM_PIXEL_BUFFS];
	CRITICAL_SECTION RetuneSection;

}  dp, *DP;

extern DP pdisp[];

extern __declspec( dllexport )
void CreateAnalyzer (	int disp,
						int *success,
						char *app_data_path);

extern __declspec( dllexport )
void XCreateAnalyzer (	int disp,
						int *success,	//writes '0' to success if all went well, <0 if mem alloc failed
						int m_size,		//maximum fft size to be used
						int m_LO,		//maximum number of LO positions per subspan
						int m_stitch,	//maximum number of subspans to be concatenated
						char *app_data_path
					 );

extern __declspec( dllexport )   
void DestroyAnalyzer(int disp);

// AetherSDR patch 24: nonzero plans this display's FFTs with FFTW_ESTIMATE.
extern __declspec( dllexport )
void SetDisplayPlanEstimate(int disp, int estimate);

// AetherSDR patch 25: retune rotation. iq_rate_hz is the real input rate;
// latency_ms the time from SetAnalyzerLo() to the first sample at that LO.
extern __declspec( dllexport )
void SetAnalyzerRetune(int disp, int enable, double iq_rate_hz, double latency_ms);

// AetherSDR patch 25: the LO (Hz) just sent to the radio. Call from the
// thread that feeds Spectrum0(), when the new LO goes out; pending_samples are
// samples already received but not yet passed to Spectrum0() (older than the
// change, so they come first).
extern __declspec( dllexport )
void SetAnalyzerLo(int disp, double lo_hz, int pending_samples);

// AetherSDR patch 25: GetPixels() plus the frame's tag: the LO it is computed
// for (Hz, 0 = unknown) and the part of the band its samples cover, Hz
// relative to that LO (vlo > vhi: too little, drop the frame).
extern __declspec( dllexport )
void GetPixelsRetune(int disp, int pixout, dOUTREAL *pix, int *flag,
					 double *lo_hz, double *vlo_hz, double *vhi_hz);

extern __declspec( dllexport )   
void SetCalibration (	int disp,
						int set_num,				//identifier for this calibration data set
						int n_points,				//number of calibration points in the set
						double (*cal)[dMAX_M+1]		//pointer to the calibration table, first
					);

extern __declspec( dllexport )   
void OpenBuffer(int disp, int ss, int LO, void **Ipointer, void **Qpointer);

extern __declspec( dllexport )   
void CloseBuffer(int disp, int ss, int LO);

extern __declspec( dllexport )
void Spectrum(int disp, int ss, int LO, dINREAL* pI, dINREAL* pQ);

extern __declspec( dllexport )
void Spectrum2(int run, int disp, int ss, int LO, dINREAL* pbuff);

extern __declspec( dllexport )
void Spectrum0(int run, int disp, int ss, int LO, double* pbuff);

extern __declspec( dllexport )
void SnapSpectrum(	int disp,
					int ss,
					int LO,
					double *snap_buff);

extern __declspec( dllexport )
void SnapSpectrumTimeout (int disp,
	                      int ss,
	                      int LO,
	                      double* snap_buff,
	                      DWORD timeout,
	                      int* flag);

#endif