

#include "signal_fm.hpp"
#include <stdio.h>//////////////////////////////////

Signal_FM::Signal_FM(float mod_idx, size_t components, float* mu, float* sigma,
                    float* weight, float max_freq, size_t tap_count, int seed,
                    float* interp_taps, size_t tap_len, int interp,
                    bool enable, size_t buff_size, size_t min_notify)
  : d_mod_idx(mod_idx),
    d_tap_count(tap_count),
    d_cum(0.),
    d_interp(interp),
    d_branch_offset(0),
    d_enable(enable),
    d_buffer_size(buff_size),
    d_notify_size(min_notify)
{
  d_rd = new boost::random_device();
  get_indicator();
  set_seed(seed);
  delete d_rd;
  //boost::mutex::scoped_lock scoped_lock(fftw_lock());
  (fftw_lock()).lock();
  d_gmm_tap_gen.set_params(components, mu, sigma, weight, 2.*max_freq, tap_count);
  generate_taps();
  (fftw_lock()).unlock();

  d_rng = new gr::random(d_seed, 0, 1);

  if(d_enable){
    d_running = true;
    auto_fill_symbols();
    auto_fill_signal();
  }
  d_first_pass = true;

  if(tap_len){
    double power_check = 0.;
    d_interp_taps = std::vector<float>(tap_len);
    for(size_t idx = 0; idx < tap_len; idx++){
      d_interp_taps[idx] = interp_taps[idx];
      power_check += interp_taps[idx]*interp_taps[idx];
    }
    double normalizer = sqrt(double(interp)/power_check);
    for(size_t idx = 0; idx < tap_len; idx++){
      d_interp_taps[idx] *= normalizer;
    }
  }
  else{
    d_interp = 1;
    tap_len = 1;
    d_interp_taps = std::vector<float>(tap_len,1.);
  }

  d_align = volk_get_alignment();
  // Generate and load the GNURadio FIR Filters with the pulse shape.
  load_firs();
  //printf("DSB::Loaded FIR filters.\n");
}

Signal_FM::~Signal_FM()
{
  delete d_rng;
  delete d_fir;
  if(d_enable){
    d_running = false;
    d_TGroup.join_all();
    delete d_Sy;
  }
  for(size_t idx = 0; idx < d_interp; idx++){
    delete d_firs[idx];
  }
}

void
Signal_FM::generate_symbols(complexf* output, size_t symbol_count)
{
  if(d_enable){
    size_t filled(0);
    while((filled < symbol_count) && d_running){
      filled += d_Sy->bmemcpy( &output[filled], symbol_count-filled, false );
    }
  }
  else{
    //printf("Generating message.\n");
    std::vector<float> message(symbol_count,0.);
    float scale = 1./3.;
    for(size_t idx = 0; idx < symbol_count; idx++){
      message[idx] = scale * d_rng->gasdev();
      output[idx] = complexf(message[idx],0.);
    }
    //printf("Generated message.\n");
    //printf("[%1.3e",message[0]);
    //for(size_t idx = 1; idx<symbol_count; idx++){
    //  printf(", %1.3e",message[idx]);
    //}
    //printf("]\n");
    //printf("[%1.3e",output[0].real());
    //for(size_t idx = 1; idx<symbol_count; idx++){
    //  printf(", %1.3e",output[idx].real());
    //}
    //printf("]\n");
  }
}

void
Signal_FM::generate_signal(complexf* output, size_t sample_count)
{
  if(d_first_pass){
    d_past2 = std::vector<complexf>(d_hist2);
    d_past = std::vector<float>(d_hist);
    d_symbol_cache = std::vector<complexf>(d_hist);
    generate_symbols( &d_symbol_cache[0], d_hist );
    for(size_t idx = 0; idx < d_hist; idx++){
      d_past[idx] = d_symbol_cache[idx].real();
    }
  }

  filter( sample_count, output );

}

void
Signal_FM::filter( size_t nout, complexf* out )
{
  size_t total_samps = d_interp*d_past2.size();
  size_t used_samps = d_branch_offset;
  size_t part_samps = (d_interp-(d_branch_offset%d_interp))%d_interp;

  total_samps -= used_samps;
  total_samps -= d_hist2*d_interp;

  size_t samps_needed;
  if(nout < total_samps){
    samps_needed = 0;
  }
  else{
    samps_needed = nout - total_samps;
  }
  float fractional_N = float(samps_needed);
  float fractional_D = float(d_interp);
  float fractional = fractional_N/fractional_D;
  size_t symbs_needed2 = ceil(fractional);
  size_t in2_len = symbs_needed2+d_past2.size();
  d_filt_in2 = (complexf*)volk_malloc( in2_len*sizeof(complexf), d_align );
  memcpy( &d_filt_in2[0], &d_past2[0], d_past2.size()*sizeof(complexf) );

  //need to first shape the gaussian input
  size_t symbs_needed;
  size_t total_input_len;
  if(d_first_pass){
    symbs_needed = symbs_needed2 + d_past2.size();
    d_past2 = std::vector<complexf>(0);
    d_first_pass = false;
  }
  else{
    symbs_needed = symbs_needed2;
  }


  total_input_len = symbs_needed + d_past.size();
  d_filt_in = (float*) volk_malloc( total_input_len*sizeof(float), d_align );
  memcpy( &d_filt_in[0], &d_past[0], d_past.size()*sizeof(float) );
  d_symbol_cache = std::vector<complexf>(symbs_needed);
  generate_symbols( &d_symbol_cache[0], symbs_needed );
  for(size_t idx = 0; idx < symbs_needed; idx++){
    d_filt_in[d_past.size()+idx] = d_symbol_cache[idx].real();
  }

  size_t oo(d_past2.size()), ii(0), oo2(0), ii2(0);
  size_t inc1(symbs_needed+d_past2.size());
  float mod = LM_2PI*d_mod_idx;

  while( oo < inc1 ){
    d_cum += d_fir->filter( &d_filt_in[ii++] );
    d_filt_in2[oo++] = std::exp(complexf( 0., mod*d_cum ));
  }
  while( oo2 < nout ){
    out[oo2] = d_firs[d_branch_offset]->filter( &d_filt_in2[ii2] );
    d_branch_offset = (d_branch_offset+1)%d_interp;
    if(!d_branch_offset){
      ii2++;
    }
    oo2++;
  }

  size_t remaining = total_input_len - ii;
  size_t remaining2 = in2_len - ii2;
  if((remaining2 < d_hist2)||(remaining < d_hist)){
    fprintf(stderr,"FM: nout(%lu), til(%lu), ii(%lu), past(%lu)\n",nout,total_input_len,ii,d_past.size());
    fprintf(stderr,"FM - There isn't enough left in the buffer!!! (%lu,%lu)\n",remaining,d_hist);
  }
  d_past = std::vector<float>( &d_filt_in[ii], &d_filt_in[total_input_len] );
  d_past2 = std::vector<complexf>( &d_filt_in2[ii2], &d_filt_in2[in2_len] );

  volk_free(d_filt_in);
  volk_free(d_filt_in2);
/*************************************************************************************
  d_symbol_cache = std::vector<complexf>(sample_count, complexf(0.,0.));
  generate_symbols( &d_symbol_cache[0], sample_count );
  double mod = LM_2PI*(d_mod_idx*d_fmax);
  for(size_t idx = 0; idx < sample_count; idx++){
    d_cum += d_symbol_cache[idx].real();
    output[idx] = exp(complexf(0.,mod*d_cum));
  }
*************************************************************************************/

}


void
Signal_FM::generate_taps()
{
  d_gmm_tap_gen.get_taps(d_taps);
  d_window = std::vector<float>(d_tap_count,0.);
  for(size_t idx = 0; idx < d_tap_count; idx++)
  {
    if(!((idx==0)||(idx==d_tap_count-1)))
    {
      d_window[idx] = 0.42 - 0.5*cos((2*M_PI*float(idx))/float(d_tap_count-1)) + 0.08*cos((4*M_PI*float(idx))/float(d_tap_count-1));
    }
    d_taps[idx] = d_taps[idx]*d_window[idx];
  }
  d_hist = d_tap_count-1;
  d_fir = new gr::filter::kernel::fir_filter_fff(1, d_taps);
}

void
Signal_FM::load_firs()
{
  std::vector<float> dummy_taps;

  size_t intp = d_interp;
  d_firs = std::vector< gr::filter::kernel::fir_filter_ccf *>(intp);
  for(size_t idx = 0; idx < intp; idx++){
    d_firs[idx] = new gr::filter::kernel::fir_filter_ccf(1,dummy_taps);
  }


  size_t leftover = (intp - (d_interp_taps.size() % intp))%intp;
  d_proto_taps = std::vector<float>(d_interp_taps.size() + leftover, 0.);
  memcpy( &d_proto_taps[0], &d_interp_taps[0],
          d_interp_taps.size()*sizeof(float) );

  d_xtaps = std::vector< std::vector<float> >(intp);
  size_t ts = d_proto_taps.size() / intp;
  for(size_t idx = 0; idx < intp; idx++){
    d_xtaps[idx].resize(ts);
  }
  for(size_t idx = 0; idx < d_proto_taps.size(); idx++){
    d_xtaps[idx % intp][idx / intp] = d_proto_taps[idx];
  }
  for(size_t idx = 0; idx < intp; idx++){
    d_firs[idx]->set_taps(d_xtaps[idx]);
  }
  d_hist2 = ts-1;
}

void
Signal_FM::auto_fill_symbols()
{
  d_Sy = new signal_threaded_buffer<complexf>(d_buffer_size,d_notify_size);

  d_TGroup.create_thread( boost::bind(&Signal_FM::auto_gen_GM, this) );

}

void
Signal_FM::auto_fill_signal()
{}


void
Signal_FM::auto_gen_GM()
{
  size_t buff_size(0), buff_pnt(0);
  float scale = 1./3.;
  std::vector<float> buffer(d_buffer_size,0.);
  std::vector<complexf> message(d_buffer_size,complexf(0.,0.));
  while(d_running){
    for(size_t idx = 0; idx < buffer.size(); idx++){
      buffer[idx] = scale * d_rng->gasdev();
      message[idx] = complexf(buffer[idx],0.);
    }
    while((buff_pnt < buffer.size()) && d_running){
      buff_pnt += d_Sy->bmemcpy( &message[buff_pnt], message.size()-buff_pnt, true );
    }
    buff_pnt = 0;
  }
}
