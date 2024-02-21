#include <cstdio>
#include <cmath>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <cassert>
#include "utils.hpp"
#include "socket.hpp"
#include "transformer.hpp"
#include <unistd.h>

#define IS_ROOT_SLICE(sliceIndex) (sliceIndex == 0)

MatmulSlice::MatmulSlice(FloatType type, int nSlices, int n, int d) {
    assert(d % nSlices == 0);

    this->type = type;
    this->nSlices = nSlices;
    this->d0 = d / nSlices;
    this->n = n;
    this->bytes = getBatchBytes(type, this->n, d);
    this->sliceBytes = getBatchBytes(type, this->n, this->d0);
}

size_t MatmulSlice::splitWeights(uint8_t sliceIndex, char* weights, char* weights0) {
    int numbersPerBatch = getNumbersPerBatch(this->type);
    int batchBytes = getBatchBytes(this->type, numbersPerBatch, 1);

    int n = this->n / numbersPerBatch;
    size_t offset = this->d0 * sliceIndex * n * batchBytes;
    size_t copiedBytes = 0;

    for (int d = 0; d < this->d0; d++) {
        for (int j = 0; j < n; j++) {
            long o = (d * n + j) * batchBytes;

            memcpy(weights0 + o, weights + offset + o, batchBytes);
            copiedBytes += batchBytes;
        }
    }
    return copiedBytes;
}

long MatmulSlice::mergeOutputs(uint8_t sliceIndex, float* output, float* output0) {
    long offset = this->d0 * sliceIndex;
    for (int i = 0; i < this->d0; i++) {
        output[offset + i] = output0[i];
    }
    return offset; // offset in floats
}

TransformerSpec Transformer::loadSpecFromFile(const char* path, const unsigned int nSlices, FloatType weightsFloatType, FloatType bufferFloatType) {
    TransformerSpec spec;
    FILE* fd = fopen(path, "rb");
    if (fd == NULL) {
        printf("Cannot open file %s\n", path);
        exit(EXIT_FAILURE);
    }

    TransformerFileHeader header;
    size_t hs = fread(&header, sizeof(TransformerFileHeader), 1, fd);
    if (hs != 1) {
        printf("Cannot read header\n");
        exit(EXIT_FAILURE);
    }

    spec.dim = header.dim;
    spec.hiddenDim = header.hiddenDim;
    spec.nLayers = header.nLayers;
    spec.nHeads = header.nHeads;
    spec.nKvHeads = header.nKvHeads;
    spec.vocabSize = abs(header.vocabSize);
    spec.seqLen = header.seqLen;
    spec.headSize = spec.dim / spec.nHeads;
    spec.kvDim = (spec.dim * spec.nKvHeads) / spec.nHeads;
    spec.weightsFloatType = weightsFloatType;
    spec.bufferFloatType = bufferFloatType;
    spec.nSlices = nSlices;

    printf("💡 dim: %d\n", spec.dim);
    printf("💡 hiddenDim: %d\n", spec.hiddenDim);
    printf("💡 nLayers: %d\n", spec.nLayers);
    printf("💡 nHeads: %d\n", spec.nHeads);
    printf("💡 nKvHeads: %d\n", spec.nKvHeads);
    printf("💡 vocabSize: %d\n", spec.vocabSize);
    printf("💡 seqLen: %d\n", spec.seqLen);
    printf("💡 nSlices: %d\n", spec.nSlices);

    fseek(fd, 0, SEEK_END);
    size_t fileSize = ftell(fd);
    fclose(fd);

    spec.fileSize = fileSize;
    return spec;
}

TransformerBuffer::TransformerBuffer(TransformerSpec* spec) {
    nSlices = spec->nSlices;
    buffers = new char*[TB_LENGTH];
    bufferBytes = new size_t[TB_LENGTH];

    bufferBytes[TB_UNIT_XB] = spec->dim * sizeof(float);
    bufferBytes[TB_UNIT_XB_QUANTIZED] = getBatchBytes(spec->bufferFloatType, spec->dim, 1);
    bufferBytes[TB_SLICED_XB2] = spec->dim * sizeof(float);
    bufferBytes[TB_SLICED_XB2_QUANTIZED] = getBatchBytes(spec->bufferFloatType, spec->dim, 1);
    bufferBytes[TB_SLICED_Q] = spec->dim * sizeof(float);
    bufferBytes[TB_SLICED_Q_QUANTIZED] = getBatchBytes(spec->bufferFloatType, spec->dim, 1);
    bufferBytes[TB_SLICED_K] = spec->kvDim * sizeof(float);
    bufferBytes[TB_SLICED_K_QUANTIZED] = getBatchBytes(spec->bufferFloatType, spec->kvDim, 1);
    bufferBytes[TB_SLICED_V] = spec->kvDim * sizeof(float);
    bufferBytes[TB_SLICED_V_QUANTIZED] = getBatchBytes(spec->bufferFloatType, spec->kvDim, 1);
    bufferBytes[TB_SLICED_HB] = spec->hiddenDim * sizeof(float);
    bufferBytes[TB_SLICED_HB_QUANTIZED] = getBatchBytes(spec->bufferFloatType, spec->hiddenDim, 1);
    for (int i = 0; i < TB_LENGTH; i += 2) {
        buffers[i] = NEW_BUFFER(bufferBytes[i]);
        if (spec->bufferFloatType == F32) {
            buffers[i + 1] = buffers[i];
        } else {
            buffers[i + 1] = NEW_BUFFER(bufferBytes[i + 1]);
        }
    }
}

TransformerBuffer::~TransformerBuffer() {
    for (int i = 0; i < TB_LENGTH; i += 2) {
        if (buffers[i] != buffers[i + 1]) {
            FREE_BUFFER(buffers[i + 1]);
        }
        FREE_BUFFER(buffers[i]);
    }
    delete[] bufferBytes;
    delete[] buffers;
}

char* TransformerBuffer::getUnit(uint8_t bufferIndex) {
    return buffers[bufferIndex];
}

size_t TransformerBuffer::getUnitBytes(uint8_t bufferIndex) {
    return bufferBytes[bufferIndex];
}

char* TransformerBuffer::getSliced(uint8_t bufferIndex, uint8_t sliceIndex) {
    size_t sliceBytes = getSlicedBytes(bufferIndex);
    return buffers[bufferIndex] + sliceBytes * sliceIndex;
}

size_t TransformerBuffer::getSlicedBytes(uint8_t bufferIndex) {
    return bufferBytes[bufferIndex] / nSlices;
}

Transformer::Transformer(TransformerSpec* spec, uint8_t sliceIndex) {
    this->spec = spec;
    this->sliceIndex = sliceIndex;

    buffer = new TransformerBuffer(spec);
    blocks = new TransformerBlock*[spec->nLayers];
    for (int i = 0; i < spec->nLayers; i++) {
        blocks[i] = new TransformerBlock(spec, sliceIndex);
    }

    if (IS_ROOT_SLICE(sliceIndex)) {
        tokenEmbeddingTableBytes = spec->vocabSize * spec->dim * sizeof(float);
        tokenEmbeddingTable = NEW_BUFFER(tokenEmbeddingTableBytes);
        rmsFinalBytes = spec->dim * sizeof(float);
        rmsFinal = NEW_BUFFER(rmsFinalBytes);
        wclsBytes = getBatchBytes(spec->weightsFloatType, spec->vocabSize, spec->dim);
        wcls = NEW_BUFFER(wclsBytes);
        x = (float*)NEW_BUFFER(spec->dim * sizeof(float));
        logits = (float*)NEW_BUFFER(spec->vocabSize * sizeof(float));
    }
}

Transformer::~Transformer() {
    delete buffer;
    for (int i = 0; i < spec->nLayers; i++) {
        delete blocks[i];
    }
    delete[] blocks;

    if (IS_ROOT_SLICE(sliceIndex)) {
        FREE_BUFFER(tokenEmbeddingTable);
        FREE_BUFFER(rmsFinal);
        FREE_BUFFER(wcls);
        FREE_BUFFER(x);
        FREE_BUFFER(logits);
    }
}

TransformerBlock::TransformerBlock(TransformerSpec* spec, uint8_t sliceIndex) {
    this->sliceIndex = sliceIndex;
    if (IS_ROOT_SLICE(sliceIndex)) {
        rmsAttBytes = spec->dim * sizeof(float);
        rmsAtt = (float*)NEW_BUFFER(rmsAttBytes);
        rmsFfnBytes = spec->dim * sizeof(float);
        rmsFfn = (float*)NEW_BUFFER(rmsFfnBytes);

        keyCache = (float*)NEW_BUFFER(spec->seqLen * spec->kvDim * sizeof(float));
        valueCache = (float*)NEW_BUFFER(spec->seqLen * spec->kvDim * sizeof(float));
        att = (float*)NEW_BUFFER(spec->nHeads * spec->seqLen * sizeof(float));
    }

    q0Slice = new MatmulSlice(spec->weightsFloatType, spec->nSlices, spec->dim, spec->dim);
    k0Slice = new MatmulSlice(spec->weightsFloatType, spec->nSlices, spec->dim, spec->kvDim);
    v0Slice = new MatmulSlice(spec->weightsFloatType, spec->nSlices, spec->dim, spec->kvDim);
    wo0Slice = new MatmulSlice(spec->weightsFloatType, spec->nSlices, spec->dim, spec->dim);
    w10Slice = new MatmulSlice(spec->weightsFloatType, spec->nSlices, spec->dim, spec->hiddenDim);
    w20Slice = new MatmulSlice(spec->weightsFloatType, spec->nSlices, spec->hiddenDim, spec->dim);
    w30Slice = new MatmulSlice(spec->weightsFloatType, spec->nSlices, spec->dim, spec->hiddenDim);

    q0 = NEW_BUFFER(q0Slice->sliceBytes);
    k0 = NEW_BUFFER(k0Slice->sliceBytes);
    v0 = NEW_BUFFER(v0Slice->sliceBytes);
    wo0 = NEW_BUFFER(wo0Slice->sliceBytes);
    w10 = NEW_BUFFER(w10Slice->sliceBytes);
    w20 = NEW_BUFFER(w20Slice->sliceBytes);
    w30 = NEW_BUFFER(w30Slice->sliceBytes);

    hb20 = (float*)NEW_BUFFER(w30Slice->d0 * sizeof(float));
}

TransformerBlock::~TransformerBlock() {
    if (IS_ROOT_SLICE(sliceIndex)) {
        FREE_BUFFER(rmsAtt);
        FREE_BUFFER(rmsFfn);
        FREE_BUFFER(keyCache);
        FREE_BUFFER(valueCache);
        FREE_BUFFER(att);
    }

    delete q0Slice;
    delete k0Slice;
    delete v0Slice;
    delete wo0Slice;
    delete w10Slice;
    delete w20Slice;
    delete w30Slice;

    FREE_BUFFER(q0);
    FREE_BUFFER(k0);
    FREE_BUFFER(v0);
    FREE_BUFFER(wo0);
    FREE_BUFFER(w10);
    FREE_BUFFER(w20);
    FREE_BUFFER(w30);

    FREE_BUFFER(hb20);
}

static size_t loadSlicedMatmulWeights(uint8_t nSlices, MatmulSlice* slice, char* weights, char* weights0, SocketPool* socketPool) {
    if (nSlices > 1) {
        char* temp = NEW_BUFFER(slice->bytes);
        memcpy(temp, weights, slice->bytes);

        size_t loadedBytes = 0;
        for (uint8_t s = 0; s < nSlices; s++) {
            uint8_t sliceIndex = (s + 1) % nSlices; // Root slice must be loaded last because we want keep root weights in the memory.
            loadedBytes += slice->splitWeights(sliceIndex, temp, weights0);
            if (sliceIndex > 0) {
                unsigned int socketIndex = sliceIndex - 1;
                socketPool->write(socketIndex, weights0, slice->sliceBytes);
            }
        }

        assert(loadedBytes == slice->bytes);
        FREE_BUFFER(temp);
        return loadedBytes;
    } else {
        size_t loadedBytes = slice->splitWeights(0, weights, weights0);
        assert(loadedBytes == slice->bytes);
        return loadedBytes;
    }
}

static size_t readSlicedMatmulWeights(MatmulSlice* slice, char* weights0, Socket* socket) {
    socket->read(weights0, slice->sliceBytes);
    return slice->sliceBytes;
}

Transformer Transformer::loadRootFromFile(const char* path, TransformerSpec* spec, SocketPool* socketPool) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        printf("Cannot open file %s\n", path);
        exit(EXIT_FAILURE);
    }
    char* data = (char*)mmap(NULL, spec->fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        printf("Mmap failed!\n");
        exit(EXIT_FAILURE);
    }
    char* weights = data + sizeof(TransformerFileHeader);
    Transformer transformer = Transformer::loadRoot(weights, spec, socketPool);
    munmap(data, spec->fileSize);
    close(fd);
    return transformer;
}

Transformer Transformer::loadRoot(char* data, TransformerSpec* spec, SocketPool* socketPool) {
    assert(socketPool->nSockets == spec->nSlices - 1);

    const uint8_t sliceIndex = 0; // Root slice
    // 创建空的主机transformer
    Transformer transformer(spec, sliceIndex);
    // 初始化从机的节点ID、模型SPEC
    if (spec->nSlices > 1) {
        for (uint8_t sliceIndex = 1; sliceIndex < spec->nSlices; sliceIndex++) {
            unsigned int socketIndex = sliceIndex - 1;
            socketPool->write(socketIndex, (char*)&sliceIndex, sizeof(uint8_t));
            socketPool->write(socketIndex, (char*)spec, sizeof(TransformerSpec));
        }
    }

    char* w = data;
    // 加载主机的tokenEmbeddingTable
    memcpy(transformer.tokenEmbeddingTable, w, transformer.tokenEmbeddingTableBytes);
    // 模型文件地址偏移
    w += transformer.tokenEmbeddingTableBytes;

    // 按照层数逐层加载attention的权重，并在加载过程中不断给从机发送数据
    for (int i = 0; i < spec->nLayers; i++) {
        TransformerBlock* block = transformer.blocks[i];
        // 加载rms的参数
        memcpy(block->rmsAtt, w, block->rmsAttBytes);
        w += block->rmsAttBytes;
        // 加载rms后线性层权重
        memcpy(block->rmsFfn, w, block->rmsFfnBytes);
        w += block->rmsFfnBytes;
        // 加载Q、K、V权重
        w += loadSlicedMatmulWeights(spec->nSlices, block->q0Slice, w, block->q0, socketPool);
        w += loadSlicedMatmulWeights(spec->nSlices, block->k0Slice, w, block->k0, socketPool);
        w += loadSlicedMatmulWeights(spec->nSlices, block->v0Slice, w, block->v0, socketPool);
        // 加载attention的线性映射的权重
        w += loadSlicedMatmulWeights(spec->nSlices, block->wo0Slice, w, block->wo0, socketPool);
        // 加载FFN的权重
        w += loadSlicedMatmulWeights(spec->nSlices, block->w10Slice, w, block->w10, socketPool);
        w += loadSlicedMatmulWeights(spec->nSlices, block->w20Slice, w, block->w20, socketPool);
        w += loadSlicedMatmulWeights(spec->nSlices, block->w30Slice, w, block->w30, socketPool);
    }

    // 加载最终rms的权重
    memcpy(transformer.rmsFinal, w, transformer.rmsFinalBytes);
    w += transformer.rmsFinalBytes;

    // 一些没有实际意义的代码
    w += (spec->seqLen * spec->headSize / 2) * sizeof(float); // skip what used to be freq_cis_real (for RoPE)
    w += (spec->seqLen * spec->headSize / 2) * sizeof(float); // skip what used to be freq_cis_imag (for RoPE)

    // 加载wcls
    memcpy(transformer.wcls, w, transformer.wclsBytes);
    w += transformer.wclsBytes;

    // 检查加载是否成功
    size_t missedBytes = (long)(w - data) - spec->fileSize + sizeof(TransformerFileHeader);
    if (missedBytes != 0) {
        printf("Missed %ld bytes\n", missedBytes);
        exit(EXIT_FAILURE);
    }
    // 加载结束
    printf("⏩ Loaded %ld bytes\n", (long)(w - data));
    return transformer;
}

Transformer Transformer::loadSlice(TransformerSpec* spec, Socket* socket) {
    uint8_t sliceIndex;
    // 等待接收主机信息
    socket->read((char*)&sliceIndex, sizeof(uint8_t));
    socket->read((char*)spec, sizeof(TransformerSpec));
    // 接收完成开始初始化
    printf("💡 sliceIndex: %d\n", sliceIndex);
    printf("💡 nSlices: %d\n", spec->nSlices);

    assert(sliceIndex >= 1);
    // 创建从机空Transformer
    Transformer transformer(spec, sliceIndex);

    // 加载矩阵权重系数
    for (int i = 0; i < spec->nLayers; i++) {
        TransformerBlock* block = transformer.blocks[i];
        size_t blockBytes = 0;
        long t0 = timeMs();
        blockBytes += readSlicedMatmulWeights(block->q0Slice, block->q0, socket);
        blockBytes += readSlicedMatmulWeights(block->k0Slice, block->k0, socket);
        blockBytes += readSlicedMatmulWeights(block->v0Slice, block->v0, socket);
        blockBytes += readSlicedMatmulWeights(block->wo0Slice, block->wo0, socket);
        blockBytes += readSlicedMatmulWeights(block->w10Slice, block->w10, socket);
        blockBytes += readSlicedMatmulWeights(block->w20Slice, block->w20, socket);
        blockBytes += readSlicedMatmulWeights(block->w30Slice, block->w30, socket);
        float kbs = blockBytes / (float)(timeMs() - t0);
        printf("⏩ Received %ld bytes for block %d (%.0f kB/s)\n", blockBytes, i, kbs);
    }
    return transformer;
}
