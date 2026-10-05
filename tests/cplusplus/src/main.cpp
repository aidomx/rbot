#include <iostream>
#include <string>

using namespace std;

int main() {
    // Deklarasi variabel
    string nama;
    int umur;

    // Output (Menampilkan teks ke layar)
    cout << "=== Program Perkenalan ===" << endl;
    cout << "Masukkan nama Anda: ";
    
    // Input (Membaca masukan dari keyboard)
    getline(cin, nama); 
    
    cout << "Masukkan umur Anda: ";
    cin >> umur;

    // Menampilkan hasil
    cout << "\nHalo, " << nama << "!" << endl;
    cout << "Ternyata umur Anda " << umur << " tahun." << endl;

    return 0;
}
