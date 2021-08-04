#include "htslib/khash.h"
#include "htslib/sam.h"
#include "htslib/vcf.h"
#include "zlib.h"

#include <deque>
#include <string>
#include <vector>

#include <unistd.h>
#include <stdio.h>

/**
>>> This is the pseudocode for the simulator
Input: coordinate-sorted bam and coordinate-sorted vcf
Output: R1 and R2 fastq files with (almost surely) different read orders.
    Please note that fastq-tools can sort these two fastq files by read name.
for each read in the sorted bam
    while the current variant is before the begin position of this read: iterate to the next variant
    calculate the hash value of the UMI
    for each variant between the begin and end positions of the read:
        if the hash value divided by the max hash value is less than the given variant allele fraction:
            modify this bam read with this variant
    flush out this bam read
**/

const double DEFAULT_ALLELE_FRAC = 0.1;

bool ispowerof2(int n) {
    return 0 == (n & (n-1));
}

double umistr2prob(const char *str, uint32_t &umihash) {
    const char *umistr1 = strchr(str, '#');
    const char *umistr = (NULL == umistr1 ? str : umistr1);
    uint32_t k = __ac_Wang_hash(__ac_X31_hash_string(umistr) ^ 0);
    umihash = k;
    return (double)(k&0xffffff) / 0x1000000;
}

struct _RevComplement {
    char data[256];
    _RevComplement() {
        for (int i = 0; i < 256; i++) {
            data[i] = (char)i;
        }
        data['A'] = 'T';
        data['T'] = 'A';
        data['C'] = 'G';
        data['G'] = 'C';
        data['a'] = 't';
        data['t'] = 'a';
        data['c'] = 'g';
        data['g'] = 'c';
    }
};

const _RevComplement THE_REV_COMPLEMENT;

void complement(std::string & astring) {
    for (int i = 0; i < astring.size(); i++) {
        auto a = astring[i];
        auto b = THE_REV_COMPLEMENT.data[a];
        astring[i] = b;
    }
}

void reverse(std::string & astring) {
    size_t sz = astring.size(); 
    for (int i = 0; i < sz / 2; i++) {
        auto tmp = astring[i];
        astring[i] = astring[sz-1-i];
        astring[sz-1-i] = tmp;
    }
}

int is_var1_before_var2(int tid1, int pos1, int tid2, int pos2) {
    return (tid1 < tid2) || (tid1 == tid2 && pos1 < pos2);
}

int bamrec_write_fastq_raw(const bam1_t *aln, gzFile &r1file, gzFile &r2file) {
    auto &outfile = ((aln->core.flag & 0x40) ? r1file : ((aln->core.flag & 0x80) ? r2file : r1file));
    std::string seq;
    std::string qual;
    
    gzprintf(outfile, "@%s\n", bam_get_qname(aln));
    
    for (int i = 0; i < aln->core.l_qseq; i++) {
        char c = seq_nt16_str[bam_seqi(bam_get_seq(aln), i)];
        seq.push_back(c);
        assert(c > ' ' || !fprintf(stderr, "The read with qname %s is invalid with qlen = %d!\n", bam_get_qname(aln), aln->core.l_qseq));
    }
    if ((aln->core.flag & 0x10)) { reverse(seq); complement(seq); }
    gzprintf(outfile, "%s\n+\n", seq.c_str());
    for (int i = 0; i < aln->core.l_qseq; i++) {
        char c = bam_get_qual(aln)[i];
        qual.push_back(c+33);
    }
    if ((aln->core.flag & 0x10)) { reverse(qual); }
    gzprintf(outfile, "%s\n", qual.c_str());
}


int bamrec_write_fastq(const bam1_t *aln, std::string &seq, std::string &qual, gzFile &r1file, gzFile &r2file) {
    auto &outfile = ((aln->core.flag & 0x40) ? r1file : ((aln->core.flag & 0x80) ? r2file : r1file));
    
    assert(qual.size() == seq.size());
    for (int i = 0; i < qual.size(); i++) {
        qual[i] = (char)(qual[i] + 33);
    }
    gzprintf(outfile, "@%s\n", bam_get_qname(aln));
    if ((aln->core.flag & 0x10)) { reverse(seq); complement(seq); }
    for (int i = 0; i < seq.size(); i++) {
        assert(seq[i] > ' ' || !fprintf(stderr, "The read with qname %s is invalid!\n", bam_get_qname(aln)));
    }
    gzprintf(outfile, "%s\n+\n", seq.c_str());
    if ((aln->core.flag & 0x10)) { reverse(qual); }
    gzprintf(outfile, "%s\n", qual.c_str());
}

void help(int argc, char **argv) {
    fprintf(stderr, "Usage: %s -b <INPUT-BAM> -v <INPUT-VCF> -1 <OUTPUT-R1-FASTQ> -2 <OUTPUT-R1-FASTQ>\n", argv[0]);
    fprintf(stderr, "Optional parameters:\n");
    fprintf(stderr, " -f fraction of variant allele (FA) to simulate. "
            "This value is overriden by the FA tag in the INPUT-VCF [default to %f].\n", DEFAULT_ALLELE_FRAC);
    fprintf(stderr, "Note:\n");
    fprintf(stderr, "INPUT-BAM and INPUT-VCF both have to be sorted and indexed\n");
    fprintf(stderr, "Each variant record in the INPUT-VCF needs to have only one variant, it cannot be multiallelic.\n");
    fprintf(stderr, "Currently, the simulation of InDel variants is not supported yet!\n");
    exit(-1);
}

int 
main(int argc, char **argv) {
    
    int flags, opt;
    char *inbam = NULL;
    char *invcf = NULL;
    char *r1outfq = NULL;
    char *r2outfq = NULL;
    double defallelefrac = DEFAULT_ALLELE_FRAC;
    
    while ((opt = getopt(argc, argv, "b:v:1:2:f:")) != -1) {
        switch (opt) {
            case 'b': inbam = optarg; break;
            case 'v': invcf = optarg; break;
            case '1': r1outfq = optarg; break;
            case '2': r2outfq = optarg; break;
            case 'f': defallelefrac = atof(optarg); break;
            default: help(argc, argv);
        }
    }
    if (NULL == inbam || NULL == invcf || NULL == r1outfq || NULL == r2outfq) {
        help(argc, argv);
    }
    int64_t num_kept_reads = 0;
    int64_t num_kept_snv = 0;
    int64_t num_kept_mnv = 0;
    int64_t num_kept_ins = 0;
    int64_t num_kept_del = 0;
    int64_t num_skip_reads = 0;
    int64_t num_skip_cmatches = 0;
    
    htsFile *vcf_fp = vcf_open(invcf, "r");
    bcf_hdr_t *vcf_hdr = bcf_hdr_read(vcf_fp);
    bcf1_t *vcf_rec = bcf_init();
    vcf_rec->rid = -1;
    vcf_rec->pos = 0;
    samFile *bam_fp = sam_open(inbam, "r");
    sam_hdr_t *bam_hdr = sam_hdr_read(bam_fp);
    bam1_t *bam_rec1 = bam_init1();
    
    gzFile r1file = gzopen(r1outfq, "wb1");
    gzFile r2file = gzopen(r2outfq, "wb1");
    
    std::deque<bcf1_t*> vcf_list;
    std::vector<bam1_t*> bam_list;
    
    float bcffloats[256] = {0};
    
    int vcf_read_ret = 0;
    
    std::string newseq, newqual;
    while (sam_read1(bam_fp, bam_hdr, bam_rec1) >= 0) {
        const bam1_t *bam_rec = bam_rec1;
        if (0 != (bam_rec->core.flag & 0x900)) { continue; }
        while (is_var1_before_var2(vcf_rec->rid, vcf_rec->pos, bam_rec->core.tid, bam_rec->core.pos)) {
            if (vcf_read_ret != -1) {
                fprintf(stderr, "The variant at tid %d pos %d is before the read at tid %d pos %d, readname = %s\n", 
                    vcf_rec->rid, vcf_rec->pos, bam_rec->core.tid, bam_rec->core.pos, bam_get_qname(bam_rec));
                vcf_read_ret = vcf_read(vcf_fp, vcf_hdr, vcf_rec); // skip this variant
                fprintf(stderr, "The new prep variant is at tid %d pos %d\n", 
                    vcf_rec->rid, vcf_rec->pos);
            }
            if (vcf_read_ret < 0) { break; }
            
        }
        while (is_var1_before_var2(vcf_rec->rid, vcf_rec->pos, bam_rec->core.tid, bam_endpos(bam_rec))) {
            if (vcf_read_ret != -1) {
                fprintf(stderr, "The variant at tid %d pos %d is before the read at tid %d endpos %d, readname = %s\n", 
                    vcf_rec->rid, vcf_rec->pos, bam_rec->core.tid, bam_endpos(bam_rec), bam_get_qname(bam_rec));
                vcf_read_ret = vcf_read(vcf_fp, vcf_hdr, vcf_rec); // get this variant
                fprintf(stderr, "The new pushed variant is at tid %d pos %d\n", 
                    vcf_rec->rid, vcf_rec->pos);
            }
            if (vcf_read_ret < 0) { break; }
            vcf_list.push_back(bcf_dup(vcf_rec));
        }
        while (vcf_list.size() > 0 && is_var1_before_var2(vcf_list.front()->rid, vcf_list.front()->pos, bam_rec->core.tid, bam_rec->core.pos)) {
            fprintf(stderr, "The variant at tid %d pos %d is destroyed\n", 
                    vcf_list.front()->rid, vcf_list.front()->pos);
            bcf_destroy(vcf_list.front());
            vcf_list.pop_front();
        }
        
        const auto *seq = bam_get_seq(bam_rec);
        const auto *qual = bam_get_qual(bam_rec); 
        newseq.clear();
        newqual.clear();
        
        uint32_t umihash = 0;
        const double mutprob = umistr2prob(bam_get_qname(bam_rec), umihash);
        if (vcf_list.size() > 0) {
            int qpos = 0;
            int rpos = bam_rec->core.pos;
            auto vcf_rec_it = vcf_list.begin();
            int vcf_list_idx = 0;
            for (int i = 0; i < bam_rec->core.n_cigar; i++) {
                const auto c = bam_get_cigar(bam_rec)[i];
                const auto cigar_op = bam_cigar_op(c);
                const auto cigar_oplen = bam_cigar_oplen(c);
                if (cigar_op == BAM_CMATCH || cigar_op == BAM_CEQUAL || cigar_op == BAM_CDIFF) {
                    auto cigar_oplen1 = cigar_oplen;
                    for (int j = 0; j < cigar_oplen1; j++) {
                        while (vcf_rec_it != vcf_list.end() && is_var1_before_var2((*vcf_rec_it)->rid, (*vcf_rec_it)->pos, bam_rec->core.tid, rpos)) {
                            vcf_rec_it++;
                            vcf_list_idx++;
                        }
                        if (vcf_rec_it != vcf_list.end() && rpos == (*vcf_rec_it)->pos) {
                            auto vcf_rec_it_end = vcf_rec_it;
                            auto vcf_list_idx_end = vcf_list_idx;
                            while (vcf_rec_it_end != vcf_list.end() && ((*vcf_rec_it_end)->rid == (*vcf_rec_it)->rid && (*vcf_rec_it_end)->pos == (*vcf_rec_it)->pos)) {
                                vcf_rec_it_end++;
                                vcf_list_idx_end++;
                            }
                            // const auto & vcf_rec = vcf_list[vcf_list_idx + (((int)umihash) % (1 + vcf_list_idx_end - vcf_list_idx))];
                            // const auto & vcf_rec = *vcf_rec_it;
                            double allelefrac = (double)0;
                            bool is_mutated = false;
for (auto vcf_rec_it2 = vcf_rec_it; vcf_rec_it2 != vcf_rec_it_end; vcf_rec_it2++) {
                            const auto & vcf_rec = *vcf_rec_it2;
                            bcf_unpack(vcf_rec, BCF_UN_ALL);
                            int ndst_val = 0;
                            int valsize = bcf_get_format_int32(vcf_hdr, vcf_rec, "FA", &bcffloats, &ndst_val);
                            allelefrac += (ndst_val > 0 ? bcffloats[ndst_val - 1] : defallelefrac);
                            if (mutprob <= allelefrac) {
                                const char *newref = vcf_rec->d.allele[0];
                                const char *newalt = vcf_rec->d.allele[1];
                                if (1 == strlen(newref) && 1 == strlen(newalt)) {
                                    newseq.push_back(newalt[0]);
                                    newqual.push_back(qual[qpos]);
                                    num_kept_snv++;
                                    if (ispowerof2(num_kept_snv)) { fprintf(stderr, "The read with name %s is spiked with the snv-variant at tid %d pos %d\n", 
                                            bam_get_qname(bam_rec), vcf_rec->rid, vcf_rec->pos); }
                                } else if (strlen(newref) == strlen(newalt)) {
                                    fprintf(stderr, "Warning: the MNV at tid %d pos %d is decomposed into SNV and only the first SNV is simulated\n", 
                                            bam_rec->core.tid, bam_rec->core.pos);
                                    newseq.push_back(newalt[0]);
                                    newqual.push_back(qual[qpos]);
                                    num_kept_mnv++;
                                } else if (strlen(newref) == 1 && strlen(newalt) >  1) {
                                    for (int k = 0; k < strlen(newalt); k++) { 
                                        newseq.push_back(newalt[k]); 
                                    }
                                    newqual.push_back(qual[qpos]);
                                    for (int k = 1; k < strlen(newalt); k++) { newqual.push_back((char)(30)); }
                                    num_kept_ins++;
                                    if (ispowerof2(num_kept_ins)) { fprintf(stderr, "The read with name %s is spiked with the ins-variant at tid %d pos %d\n", 
                                                bam_get_qname(bam_rec), vcf_rec->rid, vcf_rec->pos); }
                                } else if (strlen(newref) >  1 && strlen(newalt) == 1) {
                                    if (strlen(newref) + j < cigar_oplen1) {
                                        j += strlen(newref) - 1;
                                        qpos += strlen(newref) - 1;
                                        rpos += strlen(newref) - 1;
                                        num_kept_del++;
                                        if (ispowerof2(num_kept_del)) { fprintf(stderr, "The read with name %s is spiked with the del-variant at tid %d pos %d\n", 
                                                bam_get_qname(bam_rec), vcf_rec->rid, vcf_rec->pos); }
                                    } else {
                                        newseq.push_back(seq_nt16_str[bam_seqi(seq, qpos)]);
                                        newqual.push_back(qual[qpos]);
                                    }
                                } else {
                                    fprintf(stderr, "The variant at tid %d pos %d failed to be processed!\n", bam_rec->core.tid, bam_rec->core.pos);
                                }
                                num_kept_reads++;
                                is_mutated = true;
                                break;
                            } 
}
                            if (!is_mutated) {
                                const char nuc = seq_nt16_str[bam_seqi(seq, qpos)];
                                newseq.push_back(nuc);
                                newqual.push_back(qual[qpos]);
                                num_skip_reads++;
                            }
                        } else {
                            const char nuc = seq_nt16_str[bam_seqi(seq, qpos)];
                            newseq.push_back(nuc);
                            newqual.push_back(qual[qpos]);
                            num_skip_cmatches++;
                        }
                        qpos++;
                        rpos++;
                    }
                } else if (cigar_op == BAM_CINS) {
                    for (int j = 0; j < cigar_oplen; j++) {
                        const char nuc = seq_nt16_str[bam_seqi(seq, qpos)];
                        newseq.push_back(seq_nt16_str[bam_seqi(seq, qpos)]);
                        newqual.push_back(qual[qpos]);
                        qpos++;
                    }
                } else if (cigar_op == BAM_CSOFT_CLIP) {
                    qpos += cigar_oplen; 
                } else if (cigar_op == BAM_CDEL) {
                    rpos += cigar_oplen;
                } else if (cigar_op == BAM_CHARD_CLIP) {
                    // qpos += cigar_oplen;
                } else {
                    fprintf(stderr, "The cigar code %d is invalid at tid %d pos %d for read %s!\n", 
                            cigar_op, bam_rec->core.tid, bam_rec->core.pos, bam_get_qname(bam_rec));
                    abort();
                }
            }
            bamrec_write_fastq(bam_rec, newseq, newqual, r1file, r2file);
        } else {
            bamrec_write_fastq_raw(bam_rec, r1file, r2file);    
        }
    }
    
    bam_destroy1(bam_rec1);
    bam_hdr_destroy(bam_hdr);
    sam_close(bam_fp);
    
    bcf_destroy1(vcf_rec);
    bcf_hdr_destroy(vcf_hdr);
    vcf_close(vcf_fp);
    
    gzclose(r1file);
    gzclose(r2file);

    fprintf(stderr, "In total: kept %ld read support, skipped %ld read support"
            ", and skipped %ld no-variant CMATCH cigars.\n", num_kept_reads, num_skip_reads, num_skip_cmatches);
    fprintf(stderr, "Kept %d snv read support\n", num_kept_snv);
    fprintf(stderr, "Kept %d mnv read support\n", num_kept_mnv);
    fprintf(stderr, "Kept %d insertion read support\n", num_kept_ins);
    fprintf(stderr, "Kept %d deletion read support\n", num_kept_del);
}

