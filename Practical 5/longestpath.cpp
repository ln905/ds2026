#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <sstream>
#include <algorithm>
#include <mutex>

using namespace std;

const int NUM_THREADS = 4;

void map_function(const string& text_chunk, string& local_max) {
    stringstream ss(text_chunk);
    string line;
    local_max = "";

    while (getline(ss, line)) {
        if (line.length() > local_max.length()) {
            local_max = line;
        }
    }
}

void reduce_function(const vector<string>& all_local_maxes, string& global_max) {
    global_max = "";
    for (const auto& path : all_local_maxes) {
        if (path.length() > global_max.length()) {
            global_max = path;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cout << "Usage: ./longestpath <filename>" << endl;
        return 1;
    }

    string filename = argv[1];
    ifstream file(filename);
    if (!file.is_open()) {
        cerr << "Error: Could not open file " << filename << endl;
        return 1;
    }

    cout << "[Master] Reading file: " << filename << "..." << endl;
    string content((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
    file.close();
    
    long filesize = content.length();
    if (filesize == 0) {
        cout << "[Master] Warning: File is empty." << endl;
        return 0;
    }
    cout << "[Master] File size: " << filesize << " bytes." << endl;

    vector<string> chunks(NUM_THREADS);
    size_t chunk_size = filesize / NUM_THREADS;
    
    for (int i = 0; i < NUM_THREADS; ++i) {
        if (i == NUM_THREADS - 1) {
            chunks[i] = content.substr(i * chunk_size);
        } else {
            chunks[i] = content.substr(i * chunk_size, chunk_size);
        }
    }

    cout << "[Master] Launching " << NUM_THREADS << " threads for Map phase..." << endl;
    vector<thread> threads;
    vector<string> intermediate_results(NUM_THREADS); 

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.push_back(thread(map_function, ref(chunks[i]), ref(intermediate_results[i])));
    }

    for (auto& t : threads) {
        t.join();
    }
    cout << "[Master] Map phase complete." << endl;

    cout << "[Master] Starting Reduce phase..." << endl;
    string final_result;
    reduce_function(intermediate_results, final_result);

    cout << "\n[Master] === RESULT ===" << endl;
    cout << "Longest Path found: " << final_result << endl;
    cout << "Length: " << final_result.length() << " characters." << endl;

    return 0;
}
