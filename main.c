#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/random.h>

// Konstanten:
#define HD_SIZE 4194304 // In der Beispieldatei um Eins weniger, was zwar der höchste Index des Arrays gewesen wäre, nicht aber die Länge!
#define RA_SIZE 65536   // Genau wie HD_SIZE zu klein!
#define PT_SIZE 1024    // Da HD_SIZE um eins zu niedrig war, wurde mittels HD_SIZE >> 12 = 1023 die falsche Seitenanzahl ermittelt!

#define PG_BITS 12
#define PG_SIZE (1 << PG_BITS)

#define PQ_SIZE ((RA_SIZE + 1) / PG_SIZE)

// Typen:
typedef uint32_t virt_t;
typedef int16_t page_t;
typedef int8_t frame_t;
typedef uint16_t raA_t;
typedef uint32_t hdA_t;

// Globals:
uint8_t hd_mem[HD_SIZE];
uint8_t ra_mem[RA_SIZE];

struct seitentabellen_zeile
{
    uint8_t present_bit;
    uint8_t dirty_bit;
    frame_t page_frame;
} seitentabelle[PT_SIZE];

// LRU-Queue Komponenten:
struct queue_element
{
    page_t next;
    page_t prev;
} queue[PT_SIZE];

page_t least_recently_used = -1;
page_t most_recently_used = -1;
frame_t queue_len = 0;

// Funktionen:

#pragma region addressen
/* Aufbau der virtuellen Addresse:
 *
 * [31 30 29 28 27 26 25 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10 09 08 07 06 05 04 03 02 01 00]
 * |----------unused----------|----------Seitennummer----------|--------Addresse auf Seite---------|
 */

page_t get_seitennummer(virt_t virt_address)
{
    /**
     * Berechnet die Nummer der Seite, auf der die virtuelle Adresse liegt
     */
    return (page_t)(virt_address >> PG_BITS);
}
raA_t virt_2_ram_address(virt_t virt_address)
{
    /**
     * Wandelt eine virtuelle Adresse in eine physikalische Adresse um.
     * Der Rückgabewert ist die physikalische 16 Bit Adresse.
     */
    raA_t ram_address = ((raA_t)seitentabelle[get_seitennummer(virt_address)].page_frame) << PG_BITS;
    ram_address |= (raA_t)(virt_address & (PG_SIZE - 1));
    return ram_address;
}

hdA_t hd_page_start(page_t seitennummer)
{
    /**
     * Berechnet den Index des ersten Bytes der gegebenen Seite auf dem HD
     */
    return (hdA_t)seitennummer * PG_SIZE;
}

raA_t ra_page_start(page_t seitennummer)
{
    /**
     * Berechnet den Index des ersten Bytes der gegebenen Seite auf dem RA
     */
    return ((raA_t)seitentabelle[seitennummer].page_frame) << PG_BITS;
}

#pragma endregion

#pragma region Helper functions
void copy_page(uint8_t *source_start, uint8_t *dest_start)
{
    /**
     * Kopiert PG_SIZE bytes von source zu dest (quasi memcpy mit Länge PG_SIZE)
     */

    // Lieber pointer kopieren, um nicht etwas anderes kaputt zu machen
    uint8_t *source_copy = source_start;
    uint8_t *dest_copy = dest_start;

    while (source_copy < source_start + PG_SIZE)
    {
        *(dest_copy++) = *(source_copy++);
    }
}
uint8_t is_mem_full()
{
    /**
     * Wenn der Speicher voll ist, gibt die Funktion 1 zurück;
     */
    return queue_len >= PQ_SIZE;
}
uint8_t is_present(page_t seitennummer)
{
    /**
     * Gibt das present_bit der Seite zurück
     */
    return seitentabelle[seitennummer].present_bit;
}

void set_dirty(page_t seitennummer)
{
    /**
     * Setzt das dirty_bit der Seite
     */
    seitentabelle[seitennummer].dirty_bit = 1;
}
#pragma endregion

#pragma region Queue functions

void init_queue(page_t seitennummer)
{
    /**
     * Initialisiert die Queue mit der gegebenen Seite als erstes Element.
     * Es muss nicht über alle Elemente geloopt werden, da prev und next immer zuerst gesetzt und dann gelesene werden
     */

    queue[seitennummer].next = -1;
    queue[seitennummer].prev = -1;

    least_recently_used = seitennummer;
    most_recently_used = seitennummer;
}

void enqueue(page_t seitennummer)
{
    /**
     * Fügt die gegebene Seite zum Anfang der Queue hinzu
     */
    queue[most_recently_used].prev = seitennummer;
    queue[seitennummer].next = most_recently_used;

    most_recently_used = seitennummer;
}

void bring_to_front(page_t seitennummer)
{
    /**
     * Verschiebt die gegebene Seite zum Anfang der Queue
     */
    if (most_recently_used == seitennummer)
        return;

    page_t prev = queue[seitennummer].prev;
    page_t next = queue[seitennummer].next;

    queue[next].prev = prev;
    queue[prev].next = next;
}

page_t dequeue()
{
    /**
     * Gibt den Index der letzten Seite zurück und entfernt sie aus der Queue
     */

    page_t lru = least_recently_used;
    least_recently_used = queue[least_recently_used].prev;
    queue[least_recently_used].next = -1;
    return lru;
}

#pragma endregion

#pragma region IO
void write_page_to_hd(page_t seitennummer)
{
    /**
     * Kopiert den Inhalt der gegebenen Seite an seine Position hd_mem
     */
    copy_page(&ra_mem[ra_page_start(seitennummer)], &hd_mem[hd_page_start(seitennummer)]);
}

void remove_lru_page()
{
    /**
     * Entfert die am längsten unbenutzte Seite aus der Queue und schreibt sie zurück nach hd_mem,
     * falls sie verändert wurde.
     */
    page_t lru = dequeue();

    if (seitentabelle[lru].dirty_bit)
        write_page_to_hd(lru);

    seitentabelle[lru].dirty_bit = 0;
    seitentabelle[lru].present_bit = 0;
    seitentabelle[lru].page_frame = -1;
}

void read_page_to_ra(page_t seitennummer)
{
    /**
     * Kopiert den Inhalt der gegebenen Seite nach ra_mem. Die Position in ra_mem wird
     * nach der Least-Recently-Used-Rückschreibe-Strategie mittels einer Queue bestimmt
     */

    seitentabelle[seitennummer].present_bit = 1;

    if (queue_len == 0)
        init_queue(seitennummer); // Queue ist leer, initialisiere mit gegebener Seite
    else
        enqueue(seitennummer); // Füge gegebene Seite in Queue ein

    if (is_mem_full())
    { // Alle Queue-Plätze sind gefüllt, der Platz in ra_mem der am längsten unbenutzten Seite wird recycled
        seitentabelle[seitennummer].page_frame = seitentabelle[least_recently_used].page_frame;
        remove_lru_page();
    }
    else
        seitentabelle[seitennummer].page_frame = queue_len++; // Es sind noch nicht alle Queue-Plätze gefüllt, page_frame
                                                              // wird um 1 inkrementiert

    copy_page(&hd_mem[hd_page_start(seitennummer)], &ra_mem[ra_page_start(seitennummer)]);
}

#pragma endregion

uint8_t get_data(virt_t virt_address)
{
    /**
     * Gibt ein Byte aus dem Arbeitsspeicher zurück.
     * Wenn die Seite nicht in dem Arbeitsspeicher vorhanden ist,
     * muss erst "get_page_from_hd(virt_address)" aufgerufen werden. Ein direkter Zugriff auf hd_mem[virt_address] ist VERBOTEN!
     * Die definition dieser Funktion darf nicht geaendert werden. Namen, Parameter und Rückgabewert muss beibehalten werden!
     */
    page_t seitennummer = get_seitennummer(virt_address);
    if (!is_present(seitennummer))
    {
        read_page_to_ra(seitennummer);
    }

    return ra_mem[virt_2_ram_address(virt_address)];
}

void set_data(virt_t virt_address, uint8_t value)
{
    /**
     * Schreibt ein Byte in den Arbeitsspeicher zurück.
     * Wenn die Seite nicht in dem Arbeitsspeicher vorhanden ist,
     * muss erst "get_page_from_hd(virt_address)" aufgerufen werden. Ein direkter Zugriff auf hd_mem[virt_address] ist VERBOTEN!
     */

    page_t seitennummer = get_seitennummer(virt_address);
    if (!is_present(seitennummer))
    {
        read_page_to_ra(seitennummer);
    }

    set_dirty(seitennummer);

    ra_mem[virt_2_ram_address(virt_address)] = value;
}

int main(void) // Vollständig unverändert, abgesehen von der Initialisierung der Seitentabelle!
{
    puts("test driver_");
    uint8_t hd_mem_expected[4194303];
    srand(1);
    fflush(stdout);
    for (int i = 0; i <= 4194303; i++)
    {
        uint8_t val = (uint8_t)rand();
        hd_mem[i] = val;
        hd_mem_expected[i] = val;
    }

    for (uint32_t i = 0; i < PT_SIZE; i++)
    {
        seitentabelle[i].dirty_bit = 0;
        seitentabelle[i].page_frame = -1;
        seitentabelle[i].present_bit = 0;
    }

    uint32_t zufallsadresse = 4192426;
    uint8_t value = get_data(zufallsadresse);

    if (hd_mem[zufallsadresse] != value)
    {
        printf("ERROR_ at Address %d, Value %d =! %d!\n", zufallsadresse, hd_mem[zufallsadresse], value);
    }

    value = get_data(zufallsadresse);

    if (hd_mem[zufallsadresse] != value)
    {
        printf("ERROR_ at Address %d, Value %d =! %d!\n", zufallsadresse, hd_mem[zufallsadresse], value);
    }
    printf("Address %d, Value %d =! %d!\n", zufallsadresse, hd_mem[zufallsadresse], value);

    srand(3);

    for (uint32_t i = 0; i <= 1000; i++)
    {
        uint32_t zufallsadresse = rand() % 4194304; // i * 4095 + 1;//rand() % 4194303
        uint8_t value = get_data(zufallsadresse);
        if (hd_mem[zufallsadresse] != value)
        {
            printf("ERROR_ at Address %d, Value %d =! %d!\n", zufallsadresse, hd_mem[zufallsadresse], value);
            for (uint32_t i = 0; i <= 1023; i++)
            {
                // printf("%d,%d-",i,seitentabelle[i].present_bit);
                if (seitentabelle[i].present_bit)
                {
                    printf("i: %d, seitentabelle[i].page_frame %d\n", i, seitentabelle[i].page_frame);
                    fflush(stdout);
                }
            }
            exit(1);
        }
        printf("i: %d data @ %u: %d hd value: %d\n", i, zufallsadresse, value, hd_mem[zufallsadresse]);
        fflush(stdout);
    }

    srand(3);

    for (uint32_t i = 0; i <= 100; i++)
    {
        uint32_t zufallsadresse = rand() % 4095 * 7;
        uint8_t value = (uint8_t)zufallsadresse >> 1;
        set_data(zufallsadresse, value);
        hd_mem_expected[zufallsadresse] = value;
        printf("i : %d set_data address: %d - %d value at ram: %d\n", i, zufallsadresse, (uint8_t)value, ra_mem[virt_2_ram_address(zufallsadresse)]);
    }

    srand(4);
    for (uint32_t i = 0; i <= 16; i++)
    {
        uint32_t zufallsadresse = rand() % 4194304; // i * 4095 + 1;//rand() % 4194303
        uint8_t value = get_data(zufallsadresse);
        if (hd_mem_expected[zufallsadresse] != value)
        {
            printf("ERROR_ at Address %d, Value %d =! %d!\n", zufallsadresse, hd_mem[zufallsadresse], value);
            for (uint32_t i = 0; i <= 1023; i++)
            {
                // printf("%d,%d-",i,seitentabelle[i].present_bit);
                if (seitentabelle[i].present_bit)
                {
                    printf("i: %d, seitentabelle[i].page_frame %d\n", i, seitentabelle[i].page_frame);
                    fflush(stdout);
                }
            }

            exit(2);
        }
        printf("i: %d data @ %u: %d hd value: %d\n", i, zufallsadresse, value, hd_mem[zufallsadresse]);
        fflush(stdout);
    }

    srand(3);
    for (uint32_t i = 0; i <= 2500; i++)
    {
        uint32_t zufallsadresse = rand() % (4095 * 5); // i * 4095 + 1;//rand() % 4194303
        uint8_t value = get_data(zufallsadresse);
        if (hd_mem_expected[zufallsadresse] != value)
        {
            printf("ERROR_ at Address %d, Value %d =! %d!\n", zufallsadresse, hd_mem_expected[zufallsadresse], value);
            for (uint32_t i = 0; i <= 1023; i++)
            {
                // printf("%d,%d-",i,seitentabelle[i].present_bit);
                if (seitentabelle[i].present_bit)
                {
                    printf("i: %d, seitentabelle[i].page_frame %d\n", i, seitentabelle[i].page_frame);
                    fflush(stdout);
                }
            }
            exit(3);
        }
        printf("i: %d data @ %u: %d hd value: %d\n", i, zufallsadresse, value, hd_mem_expected[zufallsadresse]);
        fflush(stdout);
    }

    puts("test end");
    fflush(stdout);
    return EXIT_SUCCESS;
}
