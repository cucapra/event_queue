#include "Generator/EQueueGenerator.h"
#include "llvm/ADT/SmallVector.h"
#include <string>     

using namespace mlir;
using namespace mlir::edsc;
using namespace mlir::edsc::intrinsics;
using namespace std;



void MLIRGenImpl::scaleSimGenerator(){
  // output feature map
  int E_h = (layer_config.ifmap_height - layer_config.filter_height - layer_config.stride) / layer_config.stride;
  int E_w = (layer_config.ifmap_width - layer_config.filter_width - layer_config.stride) / layer_config.stride;
  // pixels of a conv window
  int px_per_conv = layer_config.filter_height * layer_config.filter_width * layer_config.channel;
  // ofmap px
  int px_ofmap = E_h * E_w * layer_config.num_filter;
  int e2 = E_h * E_w;
  
  
  theModule = mlir::ModuleOp::create(builder.getUnknownLoc());  
  auto f32Type = builder.getF32Type();
  auto ifmapType = MemRefType::get({layer_config.channel, layer_config.ifmap_height, layer_config.ifmap_width}, f32Type);
  auto filterType = MemRefType::get({layer_config.num_filter, layer_config.channel, layer_config.ifmap_width, layer_config.filter_width}, f32Type);
  auto ofmapType = MemRefType::get({layer_config.num_filter, E_h, E_w}, f32Type);
  auto f =
      makeFunction("graph", {}, {ifmapType, filterType});
  theModule.push_back(f);
  ScopedContext scope(builder, f.getLoc());
  
  Value proc, mem, comp;
  for(int i = 0; i < accel_config.array_height; i++){
    for(int j = 0; j < accel_config.array_width; j++){
      proc = create_proc("proc"+to_string(i)+","+to_string(j), "AIEngine");
      mem = create_mem("mem"+to_string(i)+","+to_string(j), ArrayRef<int64_t>{ 3 }, "f32", "RegisterFile", 3);
      if(i==0&&j==0) {
        comp = create_comp("pe_"+to_string(i)+","+to_string(j), ValueRange{mem, proc}) ;
      } else {
        comp = create_comp("pe_"+to_string(i)+","+to_string(j), ValueRange{mem, proc, comp});
      }
    }
  }

  Value sram(create_mem("mem", ArrayRef<int64_t>{ accel_config.ifmap_sram * 1024 }, "f32", "SRAM", accel_config.array_height + accel_config.array_width ) );
  Value dma_col;
  for(int i = 0; i < accel_config.array_width; i++){
    Value dma(create_dma("dma"+to_string(i)));
    if(i==0) {
      dma_col = create_comp("dma_col", ValueRange{dma}) ;
    }else{
      dma_col = create_comp("dma_col", ValueRange{dma, dma_col}) ;
    }
  }
  Value dma_row;//dma for row
  for(int i = 0; i < accel_config.array_height; i++){
    Value dma(create_dma("dma"+to_string(i)));
    if(i==0) {
      dma_row = create_comp("dma_row", ValueRange{dma}) ;
    }else{
      dma_row = create_comp("dma_row", ValueRange{dma, dma_row}) ;
    }
  }
  Value processor(create_proc("proc", "MicroPlate") );
  Value accel( create_comp("accel", ValueRange{ comp, processor, sram, dma_row, dma_col}) );
  Value signal = start_op();
    
  ValueRange res;
  if(accel_config.dataflow==DataFlow::OS){
    int num_v_fold = ceil( (float)layer_config.num_filter / (float)accel_config.array_width);
    int num_h_fold = ceil( (float)e2 / (float)accel_config.array_height);

    res = LaunchOpBuilder(signal, processor, ValueRange{accel, f.getArgument(0), f.getArgument(1)}, 
      [&](ValueRange ins){
        accel = ins[0];
        Value ifmap = ins[1];
        Value filter = ins[2];
        Value ofmap;
        processor = get_comp(accel, "proc");
        
        dma_row = get_comp(accel, "dma_row");
        dma_col = get_comp(accel, "dma_col");
        SmallVector<Value, 20> dma_rows, dma_cols;
        
        for(int i = accel_config.array_height-1; i >= 0 ; i--){
          dma_rows.push_back(get_comp(dma_row, "dma"+to_string(i) ));
          if(i!=0) dma_row = get_comp(dma_row, "dma_row");
        }
        for(int i = accel_config.array_width-1; i >= 0; i--){
          dma_cols.push_back(get_comp(dma_col, "dma"+to_string(i) ));
          if(i!=0) dma_col = get_comp(dma_col, "dma_col");
        }
        
        sram = get_comp(accel, "mem");
        Value ibuffer = alloc_op(sram, ArrayRef<int64_t>{layer_config.channel, layer_config.ifmap_height, layer_config.ifmap_width}, "f32", f32Type);
        Value wbuffer = alloc_op(sram, ArrayRef<int64_t>{layer_config.num_filter, layer_config.channel, layer_config.ifmap_width, layer_config.filter_width}, "f32", f32Type);
        Value obuffer = alloc_op(sram, ArrayRef<int64_t>{layer_config.num_filter, E_h, E_w}, "f32", f32Type);      
        
        //get all allocations done
        SmallVector<SmallVector<Value, 20>, 20> pes, mems, procs;
        SmallVector<SmallVector<Value, 20>, 20> wbuffer2s, obuffer2s, ibuffer2s;
        Value wbuffer2, obuffer2, ibuffer2;
        
        Value pe;
        //par for
        for(int i = accel_config.array_height-1; i >= 0; i--){
          SmallVector<Value, 20> line_pe, line_mem, line_proc;
          SmallVector<Value, 20> line_wbuffer, line_obuffer, line_ibuffer;
          for(int j = accel_config.array_width-1; j >= 0; j--){
            if(j==accel_config.array_width-1 && i==accel_config.array_height-1){
              pe = get_comp(accel, "pe_"+to_string(i)+","+to_string(j));
            }else{
              pe = get_comp(pe, "pe_"+to_string(i)+","+to_string(j));
            }
            
            mem = get_comp(pe, "mem"+to_string(i)+","+to_string(j));
            proc = get_comp(pe, "proc"+to_string(i)+","+to_string(j));
            wbuffer2 = alloc_op(mem, ArrayRef<int64_t>{ 1 }, "f32", f32Type);
            obuffer2 = alloc_op(mem, ArrayRef<int64_t>{ 1 }, "f32", f32Type);
            ibuffer2 = alloc_op(mem, ArrayRef<int64_t>{ 1 }, "f32", f32Type);// ArrayRef<int64_t>{ 4 }
            
            
            line_pe.push_back(pe);
            line_mem.push_back(mem);
            line_proc.push_back(proc);
            line_wbuffer.push_back(wbuffer2);
            line_obuffer.push_back(obuffer2);
            line_ibuffer.push_back(ibuffer2);
            
          }
          pes.push_back(line_pe);
          mems.push_back(line_mem);
          procs.push_back(line_proc);
          wbuffer2s.push_back(line_wbuffer);
          obuffer2s.push_back(line_obuffer);
          ibuffer2s.push_back(line_ibuffer);
        }

        int last_width = layer_config.num_filter % accel_config.array_width;
        if(last_width==0) last_width = accel_config.array_width;
        int last_height = e2%accel_config.array_height;
        if(last_height==0) last_height = accel_config.array_height;
        for(int t = 0; t < num_v_fold*num_h_fold*px_per_conv + 
            last_width + last_height; t++ ){
          int col_this_fold = accel_config.array_width;
          int row_this_fold = accel_config.array_height;
          //if(t>= num_v_fold*num_h_fold*px_per_conv) col_this_fold = last_width;
          //if(t>= num_v_fold*num_h_fold*px_per_conv+last_width*px_per_conv) row_this_fold = last_height;
          
          Value start_cpy = start_op();
          Value c0 = std_constant_index(0);
          Value col_cpy_end, prev_col_cpy;
          bool empty_col = true;
          bool first = true;
          for(int c = 0; c < col_this_fold; c++){
            if(t>=c && t+col_this_fold-c < num_v_fold*num_h_fold*px_per_conv + 
            last_width + last_height){
              col_cpy_end = memcpy_op(start_cpy, wbuffer, wbuffer2s[0][c], dma_cols[c], ValueRange{c0}, c, 0);
              if(!first){
                prev_col_cpy = control_and(ValueRange{col_cpy_end, prev_col_cpy});
              }else{
                prev_col_cpy = col_cpy_end;
                first = false;
              }
              empty_col = false;
            }
          }
          Value row_cpy_end, prev_row_cpy;
          bool empty_row = true;
          first = true;
          for(int r = 0; r < row_this_fold; r++){
            if(t>=r && t+row_this_fold-r < num_v_fold*num_h_fold*px_per_conv + 
            last_width + last_height){
              row_cpy_end = memcpy_op(start_cpy, ibuffer, ibuffer2s[r][0], dma_rows[r], ValueRange{c0}, col_this_fold+r, 1);
              if(!first){
                prev_row_cpy = control_and(ValueRange{row_cpy_end, prev_row_cpy});
              }else{
                prev_row_cpy = row_cpy_end;
                first = false;
              }
              empty_row = false;
            }
          }
          if(!empty_row && !empty_col) await_op(ValueRange{prev_row_cpy, prev_col_cpy});
          if(!empty_row && empty_col) await_op(ValueRange{prev_row_cpy});
          if(empty_row && !empty_col) await_op(ValueRange{prev_col_cpy});
          
          
          Value start_compute_signal = start_op();
          Value compute_signal, prev_compute_signal;
          SmallVector<SmallVector<Value, 20>, 20> ifmap_flight, filter_flight, ofmap_flight;
          for(int r = 0; r < row_this_fold; r++){//par_for
            SmallVector<Value, 20> ifmap_flight_line, filter_flight_line, ofmap_flight_line;
            for(int c = 0 ; c < col_this_fold; c++){//par_for
              auto pe_res = LaunchOpBuilder(start_compute_signal, procs[r][c], ValueRange{
                ibuffer2s[r][c], wbuffer2s[r][c], obuffer2s[r][c] }, 
                [&](ValueRange ins){
                filter = read_op(ins[1]);
                ifmap = read_op(ins[0],ValueRange{}, 1);
                ofmap = std_mulf(ifmap, filter);
                Value ofmap_old = read_op(ins[2], ValueRange{}, 2);
                ofmap = std_addf(ofmap, ofmap_old);
                return_op(ValueRange{ifmap, filter, ofmap});
              });
              
              compute_signal = pe_res[0];
              ifmap_flight_line.push_back(pe_res[1]);//c
              filter_flight_line.push_back(pe_res[2]);//c
              ofmap_flight_line.push_back(pe_res[3]);//c
              if(c==0 && r==0){
                prev_compute_signal = compute_signal;
              } else {
                prev_compute_signal = control_and(ValueRange{prev_compute_signal, compute_signal});
              }
            }
            ifmap_flight.push_back(ifmap_flight_line);//r
            filter_flight.push_back(filter_flight_line);//r
            ofmap_flight.push_back(ofmap_flight_line);//r
          }
          await_op(ValueRange{prev_compute_signal});
          
          start_compute_signal = start_op();
          //Value compute_signal, prev_compute_signal;
          for(int c = 0 ; c < col_this_fold; c++){//par_for
            for(int r = 0; r < row_this_fold; r++){//par_for
              if(c!=col_this_fold-1 && r!=row_this_fold-1){
                compute_signal = LaunchOpBuilder(start_compute_signal, procs[r][c], ValueRange{
                  ifmap_flight[r][c], filter_flight[r][c], ofmap_flight[r][c], obuffer2s[r][c], 
                  obuffer2s[r][c], ibuffer2s[r][c+1], wbuffer2s[r+1][c], obuffer}, 
                  [&](ValueRange ins){
                  ifmap = ins[0];
                  write_op(ifmap, ins[4], 1);
                  filter = ins[1];
                  write_op(filter, ins[5]);
                  ofmap = ins[2];
                  if( t-c-r > 0 && (t-c-r)%px_per_conv==0 && 
                    t+c+r<=num_v_fold*num_h_fold*px_per_conv+last_width+last_height){
                    c0 = std_constant_float(llvm::APFloat(7.0f), f32Type);
                    write_op(c0, ins[3], 2);
                    //???
                    //write_op(ofmap, ins[6], c%2);
                  }else{
                    write_op(ofmap, ins[3], 2);
                  }
                  return_op(ValueRange{});
                })[0];
              } else if(c==col_this_fold-1 && r!=row_this_fold-1){
                compute_signal = LaunchOpBuilder(start_compute_signal, procs[r][c], ValueRange{
                  filter_flight[r][c], ofmap_flight[r][c], obuffer2s[r][c], 
                  wbuffer2s[r+1][c], obuffer}, 
                  [&](ValueRange ins){
                  filter = ins[0];
                  write_op(filter, ins[3]);
                  ofmap = ins[1];
                  if( t-c-r > 0 && (t-c-r)%px_per_conv==0 && 
                    t+c+r<=num_v_fold*num_h_fold*px_per_conv+last_width+last_height){
                    c0 = std_constant_float(llvm::APFloat(7.0f), f32Type);
                    write_op(c0, ins[2], 2);
                    write_op(ofmap, ins[4], c);
                    //llvm::outs()<<"second"<<c<<"\n";
                  }else{
                    write_op(ofmap, ins[2], 2);
                  }
                  return_op(ValueRange{});
                })[0];
              } else if(c!=col_this_fold-1 && r==row_this_fold-1){
                compute_signal = LaunchOpBuilder(start_compute_signal, procs[r][c], ValueRange{
                  ifmap_flight[r][c], ofmap_flight[r][c], obuffer2s[r][c], 
                  ibuffer2s[r][c+1], obuffer}, // a little violation of resource control
                  [&](ValueRange ins){
                  ifmap = ins[0];
                  write_op(ifmap, ins[3], 1);
                  ofmap = ins[1];
                  if( t-c-r > 0 && (t-c-r)%px_per_conv==0 && 
                    t+c+r<=num_v_fold*num_h_fold*px_per_conv+last_width+last_height){
                    c0 = std_constant_float(llvm::APFloat(7.0f), f32Type);
                    write_op(c0, ins[2], 2);
                    write_op(ofmap, ins[4], c);
                  }else{
                    write_op(ofmap, ins[2], 2);
                  }
                  return_op(ValueRange{});
                })[0];
              } else {
                compute_signal = LaunchOpBuilder(start_compute_signal, procs[r][c], ValueRange{
                  ofmap_flight[r][c], obuffer2s[r][c], 
                  obuffer}, // a little violation of resource control
                  [&](ValueRange ins){
                  ofmap = ins[0];
                  if( t-c-r > 0 && (t-c-r)%px_per_conv==0 && 
                    t+c+r<=num_v_fold*num_h_fold*px_per_conv+last_width+last_height){
                    c0 = std_constant_float(llvm::APFloat(7.0f), f32Type);
                    write_op(c0, ins[1], 2);
                    write_op(ofmap, ins[2], c);
                  }else{
                    write_op(ofmap, ins[1], 2);
                  }
                  return_op(ValueRange{});
                })[0];
              }
              if(c==0 && r==0){
                prev_compute_signal = compute_signal;
              } else {
                prev_compute_signal = control_and(ValueRange{prev_compute_signal, compute_signal});
              }
            }
          }
          await_op(ValueRange{prev_compute_signal});
        }
        //return_op(ValueRange{obuffer});
        return_op(ValueRange{});
    });
  }
  
    // input stationary
  if(accel_config.dataflow==DataFlow::IS){
    int num_h_fold = 1;
    int max_parallel_conv = 1;
    if(accel_config.array_height < px_per_conv){
      // in case a column of PE is smaller than the size of conv window
      // |a1...f3| |g1...l1| a column has to folded into multible columns
      num_h_fold = ceil( (float)px_per_conv / (float)accel_config.array_height);
    }else{
      // we can handle multple different conv at same time
      //   |a1...m3| |c1...o3| 
      //   |b1...n3| |f1...r1| 
      max_parallel_conv = accel_config.array_height / px_per_conv;
    }
    int max_cols_per_v_fold = max_parallel_conv * accel_config.array_width;
    // e2 is the number of elements ofm (not include channels)
    // bascially calculates how many times to remap based on the input 
    // e2 is the number of columns we need, and max_cols_per_v_fold is the number of 'columns' we have
    int num_v_fold = ceil((float)e2/ (float)max_cols_per_v_fold);
    // initialze number of remaining columns
    auto remaining_cols = e2;
    // ------------------------------------------------------
    // setting up memories for input, output, weight. set up for processors and pe array
    // ---------------------------------------------------
    auto res = LaunchOpBuilder(signal, processor, ValueRange{accel, f.getArgument(0), f.getArgument(1)}, 
    [&](ValueRange ins){
      accel = ins[0];
      Value ifmap = ins[1];
      Value filter = ins[2];
      Value ofmap;
      processor = get_comp(accel, "proc");
      // dma copy and paste between memories, allow parallel 
      dma_row = get_comp(accel, "dma_row");
      dma_col = get_comp(accel, "dma_col");
      SmallVector<Value, 20> dma_rows, dma_cols;
      
      // dmas associated with r/cs
      for(int i = accel_config.array_height-1; i >= 0 ; i--){
        dma_rows.push_back(get_comp(dma_row, "dma"+to_string(i) ));
        if(i!=0) dma_row = get_comp(dma_row, "dma_row");
      }
      for(int i = accel_config.array_width-1; i >= 0; i--){
        dma_cols.push_back(get_comp(dma_col, "dma"+to_string(i) ));
        if(i!=0) dma_col = get_comp(dma_col, "dma_col");
      }
      
      sram = get_comp(accel, "mem");
      Value ibuffer = alloc_op(sram, ArrayRef<int64_t>{layer_config.channel, layer_config.ifmap_height, layer_config.ifmap_width}, "f32", f32Type);
      Value wbuffer = alloc_op(sram, ArrayRef<int64_t>{layer_config.num_filter, layer_config.channel, layer_config.ifmap_width, layer_config.filter_width}, "f32", f32Type);
      Value obuffer = alloc_op(sram, ArrayRef<int64_t>{layer_config.num_filter, E_h, E_w}, "f32", f32Type);      
      
      //get all allocations done
      SmallVector<SmallVector<Value, 20>, 20> pes, mems, procs;
      SmallVector<SmallVector<Value, 20>, 20> wbuffer2s, obuffer2s, ibuffer2s;
      Value wbuffer2, obuffer2, ibuffer2;
      
      Value pe;
      //pe = get_comp(pe, "pe");
      // why loop through the size of PE array for I/OFM?
      for(int i = accel_config.array_height-1; i >= 0; i--){
      //for(int i = 0; i >= 0; i--){
        //llvm::outs()<<i<<"\n";
        SmallVector<Value, 20> line_pe, line_mem, line_proc;
        SmallVector<Value, 20> line_wbuffer, line_obuffer, line_ibuffer;
        for(int j = accel_config.array_width-1; j >= 0; j--){
          if(j==accel_config.array_width-1 && i==accel_config.array_height-1){
            pe = get_comp(accel, "pe_"+to_string(i)+","+to_string(j));
          }else{
            pe = get_comp(pe, "pe_"+to_string(i)+","+to_string(j));
          }
          
          mem = get_comp(pe, "mem"+to_string(i)+","+to_string(j));
          proc = get_comp(pe, "proc"+to_string(i)+","+to_string(j));
          wbuffer2 = alloc_op(mem, ArrayRef<int64_t>{ 1 }, "f32", f32Type);
          obuffer2 = alloc_op(mem, ArrayRef<int64_t>{ 1 }, "f32", f32Type);
          ibuffer2 = alloc_op(mem, ArrayRef<int64_t>{ 1 }, "f32", f32Type);// ArrayRef<int64_t>{ 4 }
          
          
          line_pe.push_back(pe);
          line_mem.push_back(mem);
          line_proc.push_back(proc);
          line_wbuffer.push_back(wbuffer2);
          line_obuffer.push_back(obuffer2);
          line_ibuffer.push_back(ibuffer2);
          
        }
        pes.push_back(line_pe);
        mems.push_back(line_mem);
        procs.push_back(line_proc);
        wbuffer2s.push_back(line_wbuffer);
        obuffer2s.push_back(line_obuffer);
        ibuffer2s.push_back(line_ibuffer);
      }
    // -----------------------------------------------------------------------
    // start moving Input in, for each fold (v_fold, h_fold) pair, recopy corresponding input
    // -----------------------------------------------------------------------
      for(int i = 0; i < num_v_fold; i++){//seq_for  
            int col_this_fold = min(remaining_cols, max_parallel_conv * accel_config.array_width);
            remaining_cols -= col_this_fold;
            int rem_h = px_per_conv;
            for(int j = 0; j < num_h_fold; j++){//seq_for
              int row_this_fold = min(rem_h, accel_config.array_height);
              rem_h-=row_this_fold;

              Value c0 = std_constant_index(0);
              Value start_cpy;  
              Value input_cpy, prev_input_cpy;
              // loop through the PE row
              for(int t = 0; t < row_this_fold-1; t++){//seq_for
                start_cpy = start_op();
                SmallVector<SmallVector<Value, 20>, 20> ifmap_flight;
                //-----------------------------------
                // read
                //----------------------------------
                for(int r = 0; r < row_this_fold-1; r++){//par_for
                  // one row
                  SmallVector<Value, 20> ifmap_flight_line;
                  for(int c = 0; c < col_this_fold; c++){//par_for
                    // read from buffer
                    auto pe_res = LaunchOpBuilder(start_cpy, procs[r][c], ValueRange{ibuffer2s[r][c]}, 
                      [&](ValueRange ins){
                      ifmap = read_op(ins[0], ValueRange{});
                      return_op(ValueRange{ifmap});
                    });
                
                    input_cpy = pe_res[0]; 
                    ifmap_flight_line.push_back(pe_res[1]);
                    if(c==0 && r==0){
                      prev_input_cpy=control_and(ValueRange{input_cpy});
                    }else{
                      prev_input_cpy = control_and(ValueRange{prev_input_cpy, input_cpy});
                    }
                  }
                  ifmap_flight.push_back(ifmap_flight_line);
                }
                await_op(ValueRange{prev_input_cpy});
                
                start_cpy = start_op();
                //------------------------------
                // write and move values
                //--------------------------------
                for(int r = 0; r < row_this_fold-1; r++){//par_for
                  for(int c = 0; c < col_this_fold; c++){//par_for
                    input_cpy = LaunchOpBuilder(start_cpy, procs[r][c], ValueRange{ifmap_flight[r][c], ibuffer2s[r+1][c]}, 
                      [&](ValueRange ins){
                      write_op(ins[0], ins[1]);
                      return_op(ValueRange{});
                    })[0];
                    if(c==0 && r==0){
                      prev_input_cpy=control_and(ValueRange{input_cpy});
                    }else{
                      prev_input_cpy = control_and(ValueRange{prev_input_cpy, input_cpy});
                    }
                  }
                }
                await_op(ValueRange{prev_input_cpy});
                
                //------------------------------
                // memcpy from sram to reg
                //--------------------------------
                start_cpy = start_op();
                // this loop seems to cpy the first row
                for(int c = 0; c < col_this_fold; c++){//par_for
                  input_cpy = memcpy_op(start_cpy, ibuffer, ibuffer2s[0][c], dma_cols[c], ValueRange{c0}, c, 0);//c,0
                  if(c==0){
                    prev_input_cpy=control_and(ValueRange{input_cpy});
                  }else{
                    prev_input_cpy = control_and(ValueRange{prev_input_cpy, input_cpy});
                  }
                }
                await_op(ValueRange{prev_input_cpy});
                
              }
              
              // computing starts
              //--------------------------------------------------
              // first move weights in, num here denotes the number of weights to be copied in, for one cycle
              //----------------------------------------------------
              Value num;
              for(int i = 0; i < e2 + accel_config.array_width + accel_config.array_height; i++){
                Value compute_signal, prev_compute_signal;
                int num = min(i, accel_config.array_height);
                if (i > e2 + accel_config.array_height){
                    num = 0;
                  }
                // copying weights in
                Value start_weight_copy = start_op();
                Value weight_cpy,  prev_weight_cpy;
                for(int n = 0; num ; n++){
                  weight_cpy = memcpy_op(start_weight_copy, wbuffer, wbuffer2s[n][0], dma_cols[n], ValueRange{c0}, n, 0);//c,0
                  if(n==0){
                    prev_weight_cpy=control_and(ValueRange{weight_cpy});
                  }else{
                    prev_weight_cpy = control_and(ValueRange{prev_weight_cpy, weight_cpy});
                  }
                }
                await_op(ValueRange{prev_input_cpy});
                //---------------------------------
                // read out current values in PE
                //---------------------------------
                Value start_compute_signal = start_op();
                SmallVector<SmallVector<Value, 20>, 20> ifmap_flight, ofmap_flight;
                for(int r = 0; r < row_this_fold; r++){//par_for
                  SmallVector<Value, 20> ifmap_flight_line, ofmap_flight_line;
                  for(int c = 0 ; c < col_this_fold; c++){//par_for
                    // one cycle for a pe
                    auto pe_res = LaunchOpBuilder(start_compute_signal, procs[r][c], ValueRange{
                      ibuffer2s[r][c], wbuffer2s[r][c], obuffer2s[r][c]}, 
                      [&](ValueRange ins){
                      ifmap = read_op(ins[0]);
                      filter = read_op(ins[1], ValueRange{}, 1);
                      ofmap = std_mulf(ifmap, filter);
                      Value ofmap_old = read_op(ins[2], ValueRange{}, 2);
                      ofmap = std_addf(ofmap, ofmap_old);
                      // precalculated
                      // ofmap[r][c] = offmap[r][c] + w[r][c] * i[r][c] 
                      return_op(ValueRange{ifmap, ofmap});
                    });
                    compute_signal = pe_res[0];
                    ifmap_flight_line.push_back(pe_res[1]);
                    ofmap_flight_line.push_back(pe_res[2]);
                    
                    if(c==0 && r==0){
                      prev_compute_signal = compute_signal;
                    } else {
                      prev_compute_signal = control_and(ValueRange{prev_compute_signal, compute_signal});
                    }
                  }
                  ifmap_flight.push_back(ifmap_flight_line);
                  ofmap_flight.push_back(ofmap_flight_line);
                }
                await_op(ValueRange{prev_compute_signal});

                // -------------------------------------------
                // value writeback and handle outputs
                // loop over the flight_map
                //-------------------------------------------
                start_compute_signal = start_op();
                for(int r = 0; r < row_this_fold; r++){//par_for
                  for(int c = 0 ; c < col_this_fold; c++){//par_for
                    // one cycle for a pe
                    // case 1: nothing is outof bound
                    if(c!=col_this_fold-1 && r!=row_this_fold-1){
                      compute_signal = LaunchOpBuilder(start_compute_signal, procs[r][c], ValueRange{
                        ifmap_flight[r][c], ofmap_flight[r][c], 
                        ibuffer2s[r][c+1], obuffer2s[r+1][c], obuffer}, 
                        [&](ValueRange ins){
                        ifmap = ins[0];
                        // write ifmap_flight[r][c] to ibuffer2s[r][c+1]
                        write_op(ifmap, ins[2]);
                        ofmap = ins[1];
                        // write ofmap_flight[r][c] to obuffer2s[r+1][c]
                        write_op(ofmap, ins[3], 2);
                        // no output is here, because we are not at the end of the row
                        return_op(ValueRange{});
                      })[0];
                      // case2: reach last column, we only consider propogating rows
                    } else if( c==col_this_fold-1 && r!=row_this_fold-1 ){// compute
                      compute_signal = LaunchOpBuilder(start_compute_signal, procs[r][c], ValueRange{
                        ofmap_flight[r][c], 
                        obuffer2s[r+1][c], obuffer}, 
                        [&](ValueRange ins){
                        ofmap = ins[0];
                        // write ofmap_flight[r][c] to obuffer2s[r+1][c]
                        write_op(ofmap, ins[1], 2);  
                        // no output is here, because we are not at the end of the                  
                        return_op(ValueRange{});
                      })[0];
                      // at the end of a row, might produce an output
                      // window for col c to output arr_h + c <= t <= total rounds - arr_w + c (first c stops outputing earlier)
                    } else if( c!=col_this_fold-1 && r==row_this_fold-1 ){
                      compute_signal = LaunchOpBuilder(start_compute_signal, procs[r][c], ValueRange{
                        ifmap_flight[r][c], ofmap_flight[r][c], 
                        ibuffer2s[r][c+1], obuffer}, 
                        [&](ValueRange ins){
                        ifmap = ins[0];
                        // write ifmap_flight[r][c] to ibuffer2s[r][c+1] 
                        write_op(ifmap, ins[2]);
                        // handling output condition
                        ofmap = ins[1];
                        if( r+c <= i && // too early, nothing to pass
                            i <= e2 + accel_config.array_height + c// too late, nothing to pass
                        ){
                          write_op(ofmap, ins[3], c);
                        }
                        return_op(ValueRange{});
                      })[0];
                    // at the end of a row, might produce an output
                    } else { //( c==col_this_fold-1 && r==row_this_fold-1 )
                      compute_signal = LaunchOpBuilder(start_compute_signal, procs[r][c], ValueRange{
                        ofmap_flight[r][c], 
                        obuffer}, 
                        [&](ValueRange ins){
                        ofmap = ins[0];
                        if( r+c <= i && // too early, nothing to pass
                            i <= e2 + accel_config.array_height + c// too late, nothing to pass
                        ){
                          write_op(ofmap, ins[1], c);
                        }
                        return_op(ValueRange{});
                      })[0];
                    }
                    if(c==0 && r==0){
                      prev_compute_signal = compute_signal;
                    } else {
                      prev_compute_signal = control_and(ValueRange{prev_compute_signal, compute_signal});
                    }
                  }
                }
                await_op(ValueRange{prev_compute_signal});             
              }   
            }
        }
         return_op(ValueRange{});
    });
  }
    
    if(accel_config.dataflow==DataFlow::WS){
      int e_fresh_px = E_h * layer_config.num_filter;
      
      // fold is number of times it takes to finish computation on some dimensions
      int num_h_fold = 1;
      int max_parallel_conv = 1;
      if(accel_config.array_height < px_per_conv){
        num_h_fold = ceil( (float)px_per_conv / (float)accel_config.array_height);
      }else{
        max_parallel_conv = accel_config.array_height / px_per_conv;
      }

      int max_cols_per_v_fold = max_parallel_conv * accel_config.array_width;
      int num_v_fold = ceil( (float)layer_config.num_filter / (float)max_cols_per_v_fold);
      
      //llvm::outs()<<num_v_fold<<" "<<num_h_fold<<"\n";//1, 3
      auto remaining_cols = layer_config.num_filter;
      
      //isolated from above
      res = LaunchOpBuilder(signal, processor, ValueRange{accel, f.getArgument(0), f.getArgument(1)}, 
        [&](ValueRange ins){
          accel = ins[0];
          Value ifmap = ins[1];
          Value filter = ins[2];
          Value ofmap;
          processor = get_comp(accel, "proc");
          
          dma_row = get_comp(accel, "dma_row");
          dma_col = get_comp(accel, "dma_col");
          SmallVector<Value, 20> dma_rows, dma_cols;
          
          for(int i = accel_config.array_height-1; i >= 0 ; i--){
            dma_rows.push_back(get_comp(dma_row, "dma"+to_string(i) ));
            if(i!=0) dma_row = get_comp(dma_row, "dma_row");
          }
          for(int i = accel_config.array_width-1; i >= 0; i--){
            dma_cols.push_back(get_comp(dma_col, "dma"+to_string(i) ));
            if(i!=0) dma_col = get_comp(dma_col, "dma_col");
          }
          
          sram = get_comp(accel, "mem");
          Value ibuffer = alloc_op(sram, ArrayRef<int64_t>{layer_config.channel, layer_config.ifmap_height, layer_config.ifmap_width}, "f32", f32Type);
          Value wbuffer = alloc_op(sram, ArrayRef<int64_t>{layer_config.num_filter, layer_config.channel, layer_config.ifmap_width, layer_config.filter_width}, "f32", f32Type);
          Value obuffer = alloc_op(sram, ArrayRef<int64_t>{layer_config.num_filter, E_h, E_w}, "f32", f32Type);      
          
          //get all allocations done
          SmallVector<SmallVector<Value, 20>, 20> pes, mems, procs;
          SmallVector<SmallVector<Value, 20>, 20> wbuffer2s, obuffer2s, ibuffer2s;
          Value wbuffer2, obuffer2, ibuffer2;
          
          Value pe;
          //pe = get_comp(pe, "pe");
          //par for
          for(int i = accel_config.array_height-1; i >= 0; i--){
          //for(int i = 0; i >= 0; i--){
            //llvm::outs()<<i<<"\n";
            SmallVector<Value, 20> line_pe, line_mem, line_proc;
            SmallVector<Value, 20> line_wbuffer, line_obuffer, line_ibuffer;
            for(int j = accel_config.array_width-1; j >= 0; j--){
              if(j==accel_config.array_width-1 && i==accel_config.array_height-1){
                pe = get_comp(accel, "pe_"+to_string(i)+","+to_string(j));
              }else{
                pe = get_comp(pe, "pe_"+to_string(i)+","+to_string(j));
              }
              
              mem = get_comp(pe, "mem"+to_string(i)+","+to_string(j));
              proc = get_comp(pe, "proc"+to_string(i)+","+to_string(j));
              wbuffer2 = alloc_op(mem, ArrayRef<int64_t>{ 1 }, "f32", f32Type);
              obuffer2 = alloc_op(mem, ArrayRef<int64_t>{ 1 }, "f32", f32Type);
              ibuffer2 = alloc_op(mem, ArrayRef<int64_t>{ 1 }, "f32", f32Type);// ArrayRef<int64_t>{ 4 }
              
              
              line_pe.push_back(pe);
              line_mem.push_back(mem);
              line_proc.push_back(proc);
              line_wbuffer.push_back(wbuffer2);
              line_obuffer.push_back(obuffer2);
              line_ibuffer.push_back(ibuffer2);
              
            }
            pes.push_back(line_pe);
            mems.push_back(line_mem);
            procs.push_back(line_proc);
            wbuffer2s.push_back(line_wbuffer);
            obuffer2s.push_back(line_obuffer);
            ibuffer2s.push_back(line_ibuffer);
          }
          
          //for ofmap computation
          for(int i = 0; i < num_v_fold; i++){//seq_for  
            int col_this_fold = min(remaining_cols, max_parallel_conv * accel_config.array_width);
            remaining_cols -= col_this_fold;
            int rem_h = px_per_conv;
            for(int j = 0; j < num_h_fold; j++){//seq_for
              int row_this_fold = min(rem_h, accel_config.array_height);
              rem_h-=row_this_fold;
              
              /// ===========================================
              /// ----------- one iteration -----------------
              /// ===========================================
              
              /// ===========================================
              /// -------- copy weights to PE array ---------
              /// ===========================================
              Value c0 = std_constant_index(0);
              Value start_cpy;
              
              Value filter_cpy, prev_filter_cpy;
              for(int t = 0; t < row_this_fold-1; t++){//seq_for
                
                start_cpy = start_op();
                SmallVector<SmallVector<Value, 20>, 20> ifmap_flight;
                for(int r = 0; r < row_this_fold-1; r++){//par_for
                  SmallVector<Value, 20> ifmap_flight_line;
                  for(int c = 0; c < col_this_fold; c++){//par_for
                    auto pe_res = LaunchOpBuilder(start_cpy, procs[r][c], ValueRange{wbuffer2s[r][c]}, 
                      [&](ValueRange ins){
                      filter = read_op(ins[0], ValueRange{});
                      return_op(ValueRange{filter});
                    });
                    filter_cpy = pe_res[0];
                    ifmap_flight_line.push_back(pe_res[1]);
                    if(c==0 && r==0){
                      prev_filter_cpy=control_and(ValueRange{filter_cpy});
                    }else{
                      prev_filter_cpy = control_and(ValueRange{prev_filter_cpy, filter_cpy});
                    }
                  }
                  ifmap_flight.push_back(ifmap_flight_line);
                }
                await_op(ValueRange{prev_filter_cpy});
                start_cpy = start_op();
                for(int r = 0; r < row_this_fold-1; r++){//par_for
                  for(int c = 0; c < col_this_fold; c++){//par_for
                    filter_cpy = LaunchOpBuilder(start_cpy, procs[r][c], ValueRange{ifmap_flight[r][c], wbuffer2s[r+1][c]}, 
                      [&](ValueRange ins){
                      write_op(ins[0], ins[1]);
                      return_op(ValueRange{});
                    })[0];
                    if(c==0 && r==0){
                      prev_filter_cpy=control_and(ValueRange{filter_cpy});
                    }else{
                      prev_filter_cpy = control_and(ValueRange{prev_filter_cpy, filter_cpy});
                    }
                  }
                }
                await_op(ValueRange{prev_filter_cpy});
                start_cpy = start_op();
                for(int c = 0; c < col_this_fold; c++){//par_for
                  filter_cpy = memcpy_op(start_cpy, wbuffer, wbuffer2s[0][c], dma_cols[c], ValueRange{c0}, c, 0);//c,0
                  if(c==0){
                    prev_filter_cpy=control_and(ValueRange{filter_cpy});
                  }else{
                    prev_filter_cpy = control_and(ValueRange{prev_filter_cpy, filter_cpy});
                  }
                }
                await_op(ValueRange{prev_filter_cpy});
              }
                  
              
              for(int t = 0; t < e2+row_this_fold+col_this_fold; t++){//seq_for
              //one parallel cycle
                
                // copy from sram to pe (ibuffer)
                start_cpy = start_op();
                Value compute_signal, prev_compute_signal;;
                //for(int c = 0 ; c < col_this_fold; c++){//par_for
                if (t == 0) {
                  for(int r = 0; r < row_this_fold; r++){//par_for
                    compute_signal = memcpy_op(start_cpy, ibuffer, ibuffer2s[r][0], dma_rows[r], ValueRange{}, r, 0);
                    if(r==0){
                      prev_compute_signal = compute_signal;
                    } else {
                      prev_compute_signal = control_and(ValueRange{prev_compute_signal, compute_signal});
                    }
                  }
                } else if (t < e2) {
                  prev_compute_signal = memcpy_op(start_cpy, ibuffer, ibuffer2s[0][0], dma_rows[0], ValueRange{c0}, 0, 0);
                } else {
                  prev_compute_signal = start_cpy;
                }
                await_op(ValueRange{prev_compute_signal});
                
                //pe runs
                Value start_compute_signal = start_op();
                SmallVector<SmallVector<Value, 20>, 20> ifmap_flight, ofmap_flight;
                for(int r = 0; r < row_this_fold; r++){//par_for
                  SmallVector<Value, 20> ifmap_flight_line, ofmap_flight_line;
                  for(int c = 0 ; c < col_this_fold; c++){//par_for
                    // one cycle for a pe
                    auto pe_res = LaunchOpBuilder(start_compute_signal, procs[r][c], ValueRange{
                      ibuffer2s[r][c], wbuffer2s[r][c], obuffer2s[r][c]}, 
                      [&](ValueRange ins){
                      ifmap = read_op(ins[0]);
                      filter = read_op(ins[1], ValueRange{}, 1);
                      ofmap = std_mulf(ifmap, filter);
                      Value ofmap_old = read_op(ins[2], ValueRange{}, 2);
                      ofmap = std_addf(ofmap, ofmap_old);
                      return_op(ValueRange{ifmap, ofmap});
                    });
                    compute_signal = pe_res[0];
                    ifmap_flight_line.push_back(pe_res[1]);
                    ofmap_flight_line.push_back(pe_res[2]);
                    
                    if(c==0 && r==0){
                      prev_compute_signal = compute_signal;
                    } else {
                      prev_compute_signal = control_and(ValueRange{prev_compute_signal, compute_signal});
                    }
                  }
                  ifmap_flight.push_back(ifmap_flight_line);
                  ofmap_flight.push_back(ofmap_flight_line);
                }
                await_op(ValueRange{prev_compute_signal});
                start_compute_signal = start_op();
                for(int r = 0; r < row_this_fold; r++){//par_for
                  for(int c = 0 ; c < col_this_fold; c++){//par_for
                    // one cycle for a pe
                    if(c!=col_this_fold-1 && r!=row_this_fold-1){
                      compute_signal = LaunchOpBuilder(start_compute_signal, procs[r][c], ValueRange{
                        ifmap_flight[r][c], ofmap_flight[r][c], 
                        ibuffer2s[r][c+1], obuffer2s[r+1][c], obuffer}, 
                        [&](ValueRange ins){
                        ifmap = ins[0];
                        write_op(ifmap, ins[2]);
                        ofmap = ins[1];
                        if( r+c <= t && // too early, nothing to pass
                            c > t-e2-row_this_fold // too late, nothing to pass
                        ){
                          write_op(ofmap, ins[4], c);
                        } else{
                          write_op(ofmap, ins[3], 2);
                        }
                        return_op(ValueRange{});
                      })[0];
                    } else if( c==col_this_fold-1 && r!=row_this_fold-1 ){// compute
                      compute_signal = LaunchOpBuilder(start_compute_signal, procs[r][c], ValueRange{
                        ofmap_flight[r][c], 
                        obuffer2s[r+1][c], obuffer}, 
                        [&](ValueRange ins){
                        ofmap = ins[0];
                        if( r+c <= t && // too early, nothing to pass
                            c > t-e2-row_this_fold && // too late, nothing to pass
                            t%e_fresh_px == 0
                        ){
                          write_op(ofmap, ins[2], c);
                        } else{
                          write_op(ofmap, ins[1], 2);
                        }
                        return_op(ValueRange{});
                      })[0];
                    } else if( c!=col_this_fold-1 && r==row_this_fold-1 ){
                      compute_signal = LaunchOpBuilder(start_compute_signal, procs[r][c], ValueRange{
                        ifmap_flight[r][c], ofmap_flight[r][c], 
                        ibuffer2s[r][c+1], obuffer}, 
                        [&](ValueRange ins){
                        ifmap = ins[0];
                        write_op(ifmap, ins[2]);
                        ofmap = ins[1];
                        if( r+c <= t && // too early, nothing to pass
                            c > t-e2-row_this_fold&&// too late, nothing to pass
                            t%e_fresh_px == 0
                        ){
                          write_op(ofmap, ins[3], c);
                        }
                        return_op(ValueRange{});
                      })[0];
                    } else { //( c==col_this_fold-1 && r==row_this_fold-1 )
                      compute_signal = LaunchOpBuilder(start_compute_signal, procs[r][c], ValueRange{
                        ofmap_flight[r][c], 
                        obuffer}, 
                        [&](ValueRange ins){
                        ofmap = ins[0];
                        if( r+c <= t && // too early, nothing to pass
                            c > t-e2-row_this_fold&&// too late, nothing to pass
                            t%e_fresh_px == 0
                        ){
                          write_op(ofmap, ins[1], c);
                        }
                        return_op(ValueRange{});
                      })[0];
                    }
                    if(c==0 && r==0){
                      prev_compute_signal = compute_signal;
                    } else {
                      prev_compute_signal = control_and(ValueRange{prev_compute_signal, compute_signal});
                    }
                  }
                }
                await_op(ValueRange{prev_compute_signal});
              }
            }
          }
        //return_op(ValueRange{obuffer});
        return_op(ValueRange{});
    });
    //builder.create<ReturnOp>(f.getLoc(), llvm::makeArrayRef(res[1]));
  }
  await_op(ValueRange{res[0]});
  std_ret();
  /// ------ end ---------
  theModule.print(llvm::outs());
  llvm::outs()<<"\n";
}
