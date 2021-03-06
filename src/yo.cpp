/**
    yo, parallel I/O utilities
    Copyright (C) 2018 Adrien Devresse

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
**/


#include <iostream>
#include <algorithm>
#include <numeric>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <thread>
#include <future>


#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


#include <hadoken/format/format.hpp>
#include <hadoken/executor/thread_pool_executor.hpp>
#include <hadoken/os/env.hpp>



#include <yo/yo.hpp>

namespace fmt = hadoken::format;


constexpr std::size_t block_size = 1 << 22;  // 16Mi

namespace yo {

template<typename Function>
struct defer_exec{
    inline defer_exec(Function f) : _f(f){}
    inline defer_exec(const defer_exec &) = delete;
    inline ~defer_exec(){
        _f();
    }
    Function _f;
};



std::string version(){
    return YO_VERSION_MAJOR "." YO_VERSION_MINOR ;
}


std::size_t get_default_number_executors(){
    try{
        auto env_num_threads = hadoken::get_env("YO_NUM_THREADS");
        if(env_num_threads){
            return std::stoul(env_num_threads.get());
        }
    }catch(...){
        // invalid conversion
    }
    return std::thread::hardware_concurrency()*2;
}


std::size_t get_default_block_size(){
    try{
        auto env_bs = hadoken::get_env("YO_BLOCK_SIZE");
        if(env_bs){
            return std::stoul(env_bs.get());
        }
    }catch(...){
        // invalid conversion
    }
    return block_size;
}



options::options() :
    _threads(get_default_number_executors()),
    _block_size(get_default_block_size())

{

}

void options::set_concurrency(int threads){
    _threads = threads;
}

int options::get_concurrency() const{
    return _threads;
}

std::size_t options::get_block_size() const{
    return _block_size;
}


struct context::internal{
    internal() : executor()
    {}

    hadoken::thread_pool_executor executor;
};


context::context() : _pimpl(new internal()){

}

context::~context(){

}


int open_file(const std::string & filename, int flags){
    int fd = open(filename.c_str(), flags | O_CLOEXEC, 0755 );

    if(fd < 0){
        throw std::system_error(errno, std::generic_category(), fmt::scat("[open] ", filename));
    }
    return fd;
}


struct stat stat_file(int fd){
    struct stat st;

    if( fstat(fd, &st) < 0){
        throw std::system_error(errno, std::generic_category(), fmt::scat("[fstat]"));
    }
    return st;
}


void copy_chunk(const options & opts, int fd_dst, int fd_src, off_t offset, std::size_t size){
    const std::size_t chunk_size = opts.get_block_size();

    std::vector<char> buffer(chunk_size);

    while(size > 0){
        const std::size_t r_size = std::min(size, chunk_size);
        do{
            int status = pread(fd_src, buffer.data(), r_size, offset);
            if(status < 0 ){
                if(errno  == EINTR || errno == EAGAIN){
                    continue;
                }
                throw std::system_error(errno, std::generic_category(), fmt::scat("[pread]"));
            }
            break;
        }while(1);

        do{
            int status = pwrite(fd_dst, buffer.data(), r_size, offset);
            if(status < 0 ){
                if(errno  == EINTR || errno == EAGAIN){
                    continue;
                }
                throw std::system_error(errno, std::generic_category(), fmt::scat("[pwrite]"));
            }
            break;
        }while(1);

        offset += r_size;
        size -= r_size;
    }

}


std::string extract_filename(const std::string & src){
    auto pos = src.rfind('/');
    if(pos != std::string::npos && pos < src.size()){
        return src.substr(pos);
    }
    return src;
}

int open_file_or_in_subdir(const std::string & dst, const std::string & filename, int mode){
    try{
        return open_file(dst, mode);
    } catch(std::system_error & e){
        if(e.code() != std::error_code( EISDIR, std::generic_category() ) ){
            throw e;
        }
    }
    return open_file(fmt::scat(dst + '/' + filename), mode);
}

void context::copy_file(const options & opts, const std::string & src, const std::string & dst){
    int fd_src = open_file(src, O_RDONLY);
    int fd_dst = open_file_or_in_subdir(dst, extract_filename(src), O_WRONLY | O_CREAT);

    defer_exec<std::function<void (void)>> close_action([&](){
       close(fd_src);
       close(fd_dst);
    });

    struct stat st = stat_file(fd_src);
    const std::size_t size_file = st.st_size;

    if( ftruncate(fd_dst, size_file) < 0){
        throw std::system_error(errno, std::generic_category(), "[ftruncate]");
    }


    // get concurrency
    const std::size_t n_workers = opts.get_concurrency();
    std::vector<std::future<void>> futures;
    futures.reserve(n_workers);

    // size of block to process per worker
    const std::size_t worker_block = size_file / n_workers;

    for(std::size_t i =0; i < n_workers; ++i){
        std::size_t worker_offset = i * worker_block;
        std::size_t read_size;

        if( (i +1) == n_workers){ // last worker
            read_size = size_file - worker_offset;
        }else{
            read_size = ( (i + 1) * worker_block) - worker_offset;
        }

        futures.emplace_back(  _pimpl->executor.twoway_execute( [&opts, fd_dst, fd_src, worker_offset, read_size]{
            copy_chunk(opts, fd_dst, fd_src, off_t(worker_offset), read_size);

        }));
    }


    for(auto & f : futures){
        f.get();
    }


}

} // yo
