#include "pstring.h"
#include "clstm.h"
#include "clstmhl.h"
#include <assert.h>
#include <iostream>
#include <vector>
#include <memory>
#include <math.h>
#include <sstream>
#include <fstream>
#include <iostream>
#include <set>
#include <regex>
#include "extras.h"
#include "utils.h"

using namespace Eigen;
using namespace ocropus;
using std::vector;
using std::map;
using std::make_pair;
using std::shared_ptr;
using std::unique_ptr;
using std::cout;
using std::ifstream;
using std::set;
using std::to_string;
using std_string = std::string;
using std_wstring = std::wstring;
using std::regex;
using std::regex_replace;
#define string std_string
#define wstring std_wstring

#ifndef NODISPLAY
void show(PyServer &py, Sequence &s, int subplot = 0, int batch=0) {
  Tensor<float,2> temp;
  temp.resize(s.size(), s.rows());
  for(int i=0; i<s.size(); i++)
    for(int j=0; j<s.rows(); j++)
      temp(i,j) = s[i].v(j,batch);
  if (subplot > 0) py.evalf("subplot(%d)", subplot);
  py.imshowT(temp, "cmap=cm.hot");
}
#endif

wstring separate_chars(const wstring &s, const wstring &charsep) {
  if (charsep == L"") return s;
  wstring result;
  for (int i = 0; i < s.size(); i++) {
    if (i > 0) result.push_back(charsep[0]);
    result.push_back(s[i]);
  }
  return result;
}

struct Dataset {
  vector<string> fnames;
  wstring charsep = utf8_to_utf32(getsenv("charsep", ""));
  int size() { return fnames.size(); }
  Dataset() {}
  Dataset(string file_list) {
    readFileList(file_list);
  }
  void readFileList(string file_list) {
    read_lines(fnames, file_list);
  }
  void getCodec(Codec &codec) {
    vector<string> gtnames;
    for (auto s : fnames) gtnames.push_back(basename(s) + ".gt.txt");
    codec.build(gtnames, charsep);
  }
  void readSample(Tensor<float,2> &raw,wstring &gt,int index) {
    string fname = fnames[index];
    string base = basename(fname);
    gt = separate_chars(read_text32(base + ".gt.txt"), charsep);
    read_png(raw, fname.c_str());
    raw = -raw + Float(1);
  }
};

pair<double, double> test_set_error(CLSTMOCR &clstm, Dataset &testset) {
  double count = 0.0;
  double errors = 0.0;
  for (int test = 0; test < testset.size(); test++) {
    Tensor<float, 2> raw;
    wstring gt;
    testset.readSample(raw, gt, test);
    wstring pred = clstm.predict(raw);
    count += gt.size();
    errors += levenshtein(pred, gt);
  }
  return make_pair(count, errors);
}

int main1(int argc, char **argv) {
  int ntrain = getienv("ntrain", 10000000);
  string save_name = getsenv("save_name", "_ocr");
  int report_time = getienv("report_time", 0);

  if (argc < 2 || argc > 3) THROW("... training [testing]");
  Dataset trainingset(argv[1]);
  assert(trainingset.size()>0);
  Dataset testset;
  if (argc>2) testset.readFileList(argv[2]);
  print("got", trainingset.size(), "files,", testset.size(), "tests");

  string load_name = getsenv("load", "");

  CLSTMOCR clstm;

  if (load_name != "") {
    clstm.load(load_name);
  } else {
    Codec codec;
    trainingset.getCodec(codec);
    print("got", codec.size(), "classes");

    clstm.target_height = int(getrenv("target_height", 48));
    clstm.createBidi(codec.codec, getienv("nhidden", 100));
    clstm.setLearningRate(getdenv("lrate", 1e-4), getdenv("momentum", 0.9));
  }
  network_info(clstm.net);

  double test_error = 9999.0;
  double best_error = 1e38;

#ifndef NODISPLAY
  PyServer py;
  if (display_every > 0) py.open();
#endif
  double start_time = now();
  int start = clstm.net->attr.get("trial", getienv("start", -1)) + 1;
  if (start > 0) print("start", start);

  Trigger test_trigger(getienv("test_every", 10000), -1, start);
  test_trigger.skip0();
  Trigger save_trigger(getienv("save_every", 10000), ntrain, start);
  save_trigger.enable(save_name!="").skip0();
  Trigger report_trigger(getienv("report_every", 100), ntrain, start);
  Trigger display_trigger(getienv("display_every", 0), ntrain, start);

  for (int trial = start; trial < ntrain; trial++) {

    int sample = lrand48() % trainingset.size();
    Tensor<float,2> raw;
    wstring gt;
    trainingset.readSample(raw, gt, sample);
    wstring pred = clstm.train(raw, gt);

    if (report_trigger(trial)) {
      print(trial);
      print("TRU", gt);
      print("ALN", clstm.aligned_utf8());
      print("OUT", utf32_to_utf8(pred));
      if (trial > 0 && report_time)
        print("steptime", (now() - start_time) / report_trigger.since());
      start_time = now();
    }

#ifndef NODISPLAY
    if (display_trigger(trial)) {
      py.evalf("clf");
      show(py, clstm.net->inputs, 411);
      show(py, clstm.net->outputs, 412);
      show(py, clstm.targets, 413);
      show(py, clstm.aligned, 414);
    }
#endif

    if (test_trigger(trial)) {
      auto tse = test_set_error(clstm, testset);
      double errors = tse.first;
      double count = tse.second;
      test_error = errors / count;
      print("ERROR", trial, test_error, "   ", errors, count);
      if (test_error<best_error) {
        best_error = test_error;
        string fname = save_name + ".clstm";
        print("saving best performing network so far", fname, "error rate: ",
              best_error);
        clstm.net->attr.set("trial", trial);
        clstm.save(fname);
      }
    }

    if (save_trigger(trial)) {
      string fname = save_name + "-" + to_string(trial) + ".clstm";
      print("saving", fname);
      clstm.net->attr.set("trial", trial);
      clstm.save(fname);
    }
  }

  return 0;
}

int main(int argc, char **argv) {
  TRY { return main1(argc, argv); }
  CATCH(const char *message) { cerr << "FATAL: " << message << endl; }
}
