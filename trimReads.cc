/*
 * Trim artificial base pairs within fastq reads.
 *
 * Author: Haibao Tang <htang@jcvi.org>
 * Date: 01/30/2011
 * License: BSD
 *
 * Trim given adapters using local alignments. Trimmed regions will be given
 * low phred quality (1) and then perform quality trim if asked (use -q 0 to
 * turn off).
 *
 * All quality values will deduct a CUTOFF value (specified by the user) and
 * the max sum segment within the quality string will then be used as the final
 * `trimmed` region.
 *
 * Input is a fastq file, output is a trimmed fastq file.
 */

#define SEQAN_PROFILE // enable time measurements
#include <iostream>
#include <fstream>

#include <seqan/file.h>
#include <seqan/align.h>
#include <seqan/misc/misc_cmdparser.h>

using namespace seqan;
using namespace std;

struct Options
{
    int score;
    int times;
    int quality_cutoff;
    int minimum_length;
    int quality_encoding;
    bool discard_adapter_reads;
};

const int SangerOffset = 33;
const int IlluminaOffset = 64;
// score, times, quality_cutoff, minimum_length, quality_encoding,
// discard_adapter_reads
Options DEFAULTS = {15, 4, 20, 30, IlluminaOffset, false}, opts;

int qualityTrim(CharString &seq, CharString &qual,
                unsigned &trimStart, unsigned &trimEnd,
                int deduction)
{
    /* Maximum subarray problem, described in wiki
     * using Kadane's algorithm
     *
     * http://en.wikipedia.org/wiki/Maximum_subarray_problem
     *
     */
    unsigned seqlen = length(seq);
    assert (seqlen==length(qual));

    int qv;
    int maxSum = 0; // we are not interested in negative sum
    trimStart = trimEnd = 0;
    int currentMaxSum = 0;
    int currentStartIndex = 0;

    for (unsigned j = 0; j < seqlen; j++)
    {
        // Here we deduct user specified cutoff
        qv = (int)(ordValue(qual[j]) - deduction);
        // Any base with qv higher than cutoff get +1 score, and any base with
        // qv lower than cutoff get -1 score. The target is to get a max-sum
        // segment for the 'modified' score array.
        qv = (qv > 0) - (qv < 0);

        currentMaxSum += qv;

        if (currentMaxSum > maxSum)
        {
            maxSum = currentMaxSum;
            trimStart = currentStartIndex;
            trimEnd = j;
        }
        else if (currentMaxSum < 0)
        {
            currentMaxSum = 0;
            currentStartIndex = j + 1;
        }
    }

    return maxSum;
}

int toSangerQuality(CharString &qual, CharString &qualSanger, int offsetDiff)
{
    for (unsigned j = 0; j < length(qual); j++)
        append(qualSanger, qual[j] + offsetDiff);

    return 0;
}

int main (int argc, char const * argv[])
{
    SEQAN_PROTIMESTART(loadTime);
    CommandLineParser p("trimReads");
    addOption(p, CommandLineOption('o', "outfile",
                                   "Output file name. "
                                   "(default replace suffix with .trimmed.fastq)", 
                                   OptionType::String));
    addOption(p, CommandLineOption('f', "adapterfile",
                                   "FASTA formatted file containing the adapters for removal ",
                                   OptionType::String, "adapters.fasta"));
    addOption(p, CommandLineOption('s', "score",
                                   "Minimum score to call adapter match. "
                                   "Default scoring scheme for +1 match, "
                                   "-3 for mismatch/gapOpen/gapExtension.",
                                   OptionType::Int, DEFAULTS.score));
    addOption(p, CommandLineOption('q', "quality-cutoff",
                                   "Trim low-quality regions below quality cutoff. "
                                   "The algorithm is similar to the one used by BWA "
                                   "by finding a max-sum segment within the quality string. "
                                   "Set it to 0 to skip quality trimming. ",
                                   OptionType::Int, DEFAULTS.quality_cutoff));
    addOption(p, CommandLineOption('m', "minimum-length",
                                   "Discard trimmed reads that are shorter than LENGTH.",
                                   OptionType::Int, DEFAULTS.minimum_length));
    addOption(p, CommandLineOption('Q', "quality-encoding",
                                   "Read quality encoding for input file. 64 for Illumina, "
                                   "33 for Sanger. ",
                                   OptionType::Int, DEFAULTS.quality_encoding));
    addOption(p, CommandLineOption('d', "discard-adapter-reads",
                                   "Discard reads with adapter sequences rather than trim",
                                   OptionType::Bool, DEFAULTS.discard_adapter_reads));

    addTitleLine(p, "Illumina reads trimming utility");
    addTitleLine(p, "Author: Haibao Tang <htang@jcvi.org>");
    addUsageLine(p, "[options] fastqfile");

    if (!parse(p, argc, argv))
        return 1;

    String<CharString> args = getArgumentValues(p);
    if (length(args) != 1 || isSetShort(p, 'h'))
    {
        help(p, cerr);
        return 0;
    }

    CharString infile = args[0];
    CharString outfile, adapterfile;
    if (isSetLong(p, "outfile"))
    {
        getOptionValueLong(p, "outfile", outfile);
    }
    else
    {
        outfile = infile;
        // replace the .suffix with .trimmed.fastq
        unsigned suffix_idx;
        for (suffix_idx = length(infile)-1; suffix_idx > 0; suffix_idx--)
            if (infile[suffix_idx] == '.') break;
        if (suffix_idx==0) suffix_idx = length(outfile);
        suffix(outfile, suffix_idx) = ".trimmed.fastq";
    }

    opts = DEFAULTS;
    getOptionValueLong(p, "adapterfile", adapterfile);
    getOptionValueLong(p, "score", opts.score);
    getOptionValueLong(p, "quality-cutoff", opts.quality_cutoff);
    getOptionValueLong(p, "minimum-length", opts.minimum_length);
    getOptionValueLong(p, "quality-encoding", opts.quality_encoding);
    if (isSetShort(p, 'd')) opts.discard_adapter_reads = true;

    MultiSeqFile multiSeqFile, adapterFile;
    if (argc < 2 || !open(multiSeqFile.concat, toCString(infile), OPEN_RDONLY) 
                 || !open(adapterFile.concat, toCString(adapterfile), OPEN_RDONLY))
        return 1;

    AutoSeqFormat format;
    // Guess the format of the input, although currently only fastq is supported
    guessFormat(multiSeqFile.concat, format);
    split(multiSeqFile, format);
    split(adapterFile, Fasta());

    unsigned seqCount = length(multiSeqFile);
    unsigned nadapters = length(adapterFile);
    unsigned tooShorts = 0;

    // Adapter library
    StringSet<CharString> adapterNames, adapters;
    CharString id, seq;

    for (unsigned i = 0; i < nadapters; i++)
    {
        assignSeqId(id, adapterFile[i], Fasta());   // read sequence id
        assignSeq(seq, adapterFile[i], Fasta());    // read sequence
        appendValue(adapterNames, id);
        appendValue(adapters, seq);
    }

    String<unsigned> startpos(nadapters); // keep track of adapter positions in concat string
    String<unsigned> adapterCounts(nadapters); // keep track of adapter counts

    // Concatenate all adapters
    unsigned pos = 0;
    startpos[0] = pos;
    CharString adapterdb = adapters[0];

    CharString Ns = "XXXXX"; // use X's to break alignments across adapters
    for (unsigned i = 1; i < nadapters; i++)
    {
        append(adapterdb, Ns);
        startpos[i] = length(adapterdb);
        append(adapterdb, adapters[i]);
    }

    for (unsigned i = 0; i < nadapters; i++)
        adapterCounts[i] = 0;

    Score<int> scoring(1, -3, -3, -3); // harsh penalty for mismatch and indel
    Align<CharString > ali;
    resize(rows(ali), 2);

    CharString qual;
    int deduction = opts.quality_encoding + opts.quality_cutoff;

    ofstream fout(toCString(outfile));
    unsigned int trimmed_reads = 0, discarded_adapter_reads = 0;

    for (unsigned i = 0; i < seqCount; i++)
    {
        assignSeqId(id, multiSeqFile[i], format);   // read sequence id
        assignSeq(seq, multiSeqFile[i], format);    // read sequence
        assignQual(qual, multiSeqFile[i], format);  // read ascii quality values

        assignSource(row(ali, 0), adapterdb);
        assignSource(row(ali, 1), seq);

        int score = localAlignment(ali, scoring, SmithWaterman());
        bool containAdapters = (score >= opts.score);

        if (containAdapters)
        {
            unsigned clipStart = clippedBeginPosition(row(ali, 1));
            unsigned clipEnd = clippedEndPosition(row(ali, 1));

            //cout << "Score = " << score << endl;
            //cout << ali << endl;

            for (unsigned j = clipStart; j < clipEnd; j++)
                // mark the adapter region with qual of 1
                qual[j] = (char) (1 + opts.quality_encoding);

            // find out which adapters generated the alignment
            unsigned dbStart = clippedBeginPosition(row(ali, 0));
            unsigned idx;
            for (idx = 0; idx < nadapters; idx++)
            {
                if (startpos[idx] > dbStart) break;
            }
            idx--; // bisect startpos
            adapterCounts[idx]++;
        }

        unsigned trimStart = 0, trimEnd = length(seq) - 1;
        // results are [trimStart, trimEnd] (inclusive on ends)
        if (opts.quality_cutoff > 0)
        {
            qualityTrim(seq, qual, trimStart, trimEnd, deduction);

            if ((trimEnd - trimStart + 1) < (unsigned) opts.minimum_length)
            {
                tooShorts++;
                continue;
            }
        }

        if (opts.discard_adapter_reads && containAdapters) 
        {
            discarded_adapter_reads++;
            continue;
        }

        fout << "@" << id << endl;
        fout << infix(seq, trimStart, trimEnd+1) << endl;
        fout << "+" << endl;
        fout << infix(qual, trimStart, trimEnd+1) << endl;
        trimmed_reads++;
    }

    fout.close();

    for (unsigned i = 0; i < nadapters; i++)
    {
        cerr << "[" << i <<"] " << adapterNames[i] << " found "
             << adapterCounts[i] << " times" << endl;
    }

    // Write a report of the trimming
    cerr << endl;
    cerr << "A total of " << tooShorts << " too short (trimmed length < "
         << opts.minimum_length << ") reads removed." << endl;
    if (opts.discard_adapter_reads)
    {
        cerr << "A total of " << discarded_adapter_reads 
             << " adapter reads discarded." << endl;
    }
    cerr << "A total of " << trimmed_reads << " trimmed reads are written to `" 
         << outfile << "`." << endl;
    cerr << "Processed " << seqCount << " sequences took " << SEQAN_PROTIMEDIFF(loadTime)
         << " seconds." << endl << endl;

    return 0;
}

