/* 
 * Trim artificial base pairs within fastq reads.
 *
 * Author: Haibao Tang <htang@jcvi.org>
 * Date: 01/30/2011
 * License: BSD
 *
 * Trim given adapters by using local alignments, up to N times to deal
 * with chimeric adapters; poly-ACGT tails can also be removed if asked. 
 * 
 * Trimmed regions will be given low phred quality (2) and then perform a
 * quality trim. The algorithm for quality trim is similar to bwa quality trim.
 * All quality values will deduct a CUTOFF value (specified by the user) and 
 * the max sum segment within the quality string will then be used as the final
 * `trimmed` region.
 * 
 * Input is a fastq file, output is a trimmed fastq file with Sanger encoding of
 * quality values.
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
};

Options DEFAULTS = {15, 2, 20, 15, 64}, opts;

int qualityTrim(CharString &seq, CharString &qual, 
        unsigned &trimStart, unsigned &trimEnd,
        int offset, int cutoff)
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
    	qv = (int)(ordValue(qual[j]) - offset - cutoff);
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

int main (int argc, char const * argv[])
{
	SEQAN_PROTIMESTART(loadTime);
    CommandLineParser p("trimReads");
    addOption(p, CommandLineOption('o', "outfile",
                "Output file name, uses Sanger encoding for quality. "
                "(default )", OptionType::String));
    addOption(p, CommandLineOption('s', "score", 
                "Minimum score to call adapter match. "
                "Default scoring scheme for +1 match, "
                "-3 for mismatch/gapOpen/gapExtension.",
                OptionType::Int, DEFAULTS.score));
    addOption(p, CommandLineOption('n', "times",
                "Try to remove the adapters at most COUNT times. "
                "Useful when an adapter gets appended multiple times.",
                OptionType::Int, DEFAULTS.times));
    addOption(p, CommandLineOption('q', "quality-cutoff", 
                "Trim low-quality regions below quality cutoff. "
                "The algorithm is similar to the one used by BWA "
                "by finding a max-sum segment within the quality string.", 
                OptionType::Int, DEFAULTS.quality_cutoff));
    addOption(p, CommandLineOption('m', "minimum-length", 
                "Discard trimmed reads that are shorter than LENGTH.", 
                OptionType::Int, DEFAULTS.minimum_length));
    addOption(p, CommandLineOption('Q', "quality-encoding",
                "Read quality encoding for input file. 64 for Illumina, "
                "33 for Sanger. Output will always be Sanger encoding.",
                OptionType::Int, DEFAULTS.quality_encoding));

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
    CharString outfile;
    if (isSetLong(p, "outfile"))
    {
        getOptionValueLong(p, "outfile", outfile);
    }
    else
    {
        outfile = infile;
        // replace the .suffix with .trimmed.fastq
        int suffix_idx;
        for (suffix_idx = length(infile)-1; suffix_idx >= 0; suffix_idx--)
            if (infile[suffix_idx] == '.') break;
        suffix(outfile, suffix_idx) = ".trimmed.fastq";
    }

    ofstream fout(toCString(outfile));

    opts = DEFAULTS;
    getOptionValueLong(p, "score", opts.score);
    getOptionValueLong(p, "times", opts.times);
    getOptionValueLong(p, "quality-cutoff", opts.quality_cutoff);
    getOptionValueLong(p, "minimum-length", opts.minimum_length);
    getOptionValueLong(p, "quality-encoding", opts.quality_encoding);

	MultiSeqFile multiSeqFile;
	if (argc < 2 || !open(multiSeqFile.concat, toCString(infile), OPEN_RDONLY))
		return 1;

    // Guess the format of the input, although currently only fastq is supported
	AutoSeqFormat format;
	guessFormat(multiSeqFile.concat, format);
	split(multiSeqFile, format);

	unsigned seqCount = length(multiSeqFile);
    unsigned tooShorts = 0;

    // Adapter library, default is Illumina PE library adapters
    // TODO: accept a FASTA file of adapters
    StringSet<CharString> adapters;
    appendValue(adapters, "AATGATACGGCGACCACCGAGATCTACACTCTTTCCCTACACGACGCTCTTCCGATCT");
    appendValue(adapters, "CAAGCAGAAGACGGCATACGAGATCGGTCTCGGCATTCCTGCTGAACCGCTCTTCCGATCT");
    appendValue(adapters, "AGATCGGAAGAGCGTCGTGTAGGGAAAGAGTGTAGATCTCGGTGGTCGCCGTATCATT");
    appendValue(adapters, "AGATCGGAAGAGCGGTTCAGCAGGAATGCCGAGACCGATCTCGTATGCCGTCTTCTGCTTG");
    unsigned nadapters = length(adapters);

    String<unsigned> startpos(nadapters); // keep track of adapter positions in concat string
    String<unsigned> adapterCounts(nadapters); // keep track of adapter counts

    // Concatenate all adapters
    unsigned pos = 0;
    startpos[0] = pos;
    CharString adapterdb = adapters[0];

    CharString Ns = "NNNNN"; // use N's to break alignments across adapters
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
    assignSource(row(ali, 0), adapterdb);

	CharString seq;
	CharString qual;
	CharString id;

	for (unsigned i = 0; i < seqCount; i++)
	{
		assignSeqId(id, multiSeqFile[i], format);   // read sequence id
		assignSeq(seq, multiSeqFile[i], format);    // read sequence
		assignQual(qual, multiSeqFile[i], format);  // read ascii quality values

        assignSource(row(ali, 1), seq);
        LocalAlignmentFinder<> finder(ali);
        int count = 0;
        while (localAlignment(ali, finder, scoring, opts.score, WatermanEggert()) \
                && count < opts.times)
        {
            unsigned clipStart = clippedBeginPosition(row(ali, 1));
            unsigned clipEnd = clippedEndPosition(row(ali, 1));

            for (unsigned j = clipStart; j < clipEnd; j++)
                qual[j] = (char) (2 + opts.quality_encoding);
            
            // find out which adapters generated the alignment
            unsigned dbStart = clippedBeginPosition(row(ali, 0));
            unsigned idx;  
            for (idx = 0; idx < nadapters; idx++)
            {
                if (startpos[idx] > dbStart) break;
            }
            idx--; // bisect startpos
            adapterCounts[idx]++;
            count++; 

            /*
            unsigned dbEnd = clippedEndPosition(row(ali, 0));
            cout << "Score = " << getScore(finder) << endl;
            cout << ali << endl;
            cout << seq << endl;
            cout << qual << endl << endl;
            */
        }

        unsigned trimStart = 0, trimEnd = 0;
        // results are [trimStart, trimEnd] (inclusive on ends)
        qualityTrim(seq, qual, trimStart, trimEnd,
                opts.quality_encoding, opts.quality_cutoff);

        if ((trimEnd - trimStart + 1) < (unsigned) opts.minimum_length) 
        {
            tooShorts++;
            continue;
        }

        fout << "@" << id << endl;
        fout << infix(seq, trimStart, trimEnd+1) << endl;
        fout << "+" << endl;
        fout << infix(qual, trimStart, trimEnd+1) << endl;
	}

    fout.close();

    for (unsigned i = 0; i < nadapters; i++)
    {
        cerr << "[" << i <<"] " << adapters[i] << " found "
                  << adapterCounts[i] << " times" << endl;
    }

    // Write a report of the trimming
    cerr << endl;
    cerr << "A total of " << tooShorts << " too short (trimmed length < " 
              << opts.minimum_length << ") reads removed" << endl;
    cerr << "Trimmed reads are written to `" << outfile << "`." << endl; 
    cerr << "Loading " << seqCount << " sequences took " << SEQAN_PROTIMEDIFF(loadTime)
         << " seconds." << endl << endl;

	return 0;
}
