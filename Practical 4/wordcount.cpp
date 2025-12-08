#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <sstream>
#include <algorithm>
#include <mutex>

using namespace std;

const int NUM_THREADS = 4;

string clean_word(const string& w) {
    string res = "";
    for (char c : w) {
        if (isalnum(c)) res += tolower(c);
    }
    return res;
}

//MAPPER
void map_function(const string& text_chunk, map<string, int>& local_result) {
    stringstream ss(text_chunk);
    string word;
    while (ss >> word) {
        string cleaned = clean_word(word);
        if (!cleaned.empty()) local_result[cleaned]++;
    }
}

//REDUCER
void reduce_function(const vector<map<string, int>>& all_maps, map<string, int>& final_result) {
    for (const auto& local_map : all_maps) {
        for (const auto& pair : local_map) {
            final_result[pair.first] += pair.second;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cout << "Usage: ./wordcount <filename>" << endl;
        return 1;
    }

    //INPUT
    ifstream file(argv[1]);
    if (!file.is_open()) return 1;
    string content((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
    file.close();
    cout << "[Master] Read file size: " << content.length() << " bytes." << endl;

    //SPLIT
    vector<string> chunks(NUM_THREADS);
    size_t chunk_size = content.length() / NUM_THREADS;
    for (int i = 0; i < NUM_THREADS; ++i) {
        chunks[i] = (i == NUM_THREADS - 1) ? 
            content.substr(i * chunk_size) : 
            content.substr(i * chunk_size, chunk_size);
    }

    //MAP
    cout << "[Master] Starting " << NUM_THREADS << " threads..." << endl;
    vector<thread> threads;
    vector<map<string, int>> intermediate_results(NUM_THREADS);

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.push_back(thread(map_function, ref(chunks[i]), ref(intermediate_results[i])));
    }

    for (auto& t : threads) t.join();
    cout << "[Master] Map phase complete." << endl;

    //REDUCE
    cout << "[Master] Reducing..." << endl;
    map<string, int> final_result;
    reduce_function(intermediate_results, final_result);

    //OUTPUT
    ofstream outfile("wordcount_output.txt");
    for (const auto& pair : final_result) {
        outfile << pair.first << ": " << pair.second << endl;
    }
    outfile.close();
    cout << "[Master] Success! Unique words: " << final_result.size() << endl;

    return 0;
}
