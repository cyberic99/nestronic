#include "nsf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include <esp_err.h>
#include <esp_log.h>

#include "fake6502.h"

static const char *TAG = "nsf";

#define ROM_BANK_SIZE  4096
#define ROM_BANK_COUNT 10

typedef struct {
    /* $0000 - $7FFF */
    uint8_t ram[2048];
    /* $1000 - $107F */
    uint8_t prg[128];
    /* $4000 - $4017 */
    uint8_t apu_regs[24];
    /* $5FF8 - $5FFF */
    uint8_t bank_regs[8];
    /* $FFFC - $FFFF */
    uint8_t int_vecs[6];

    /* ROM bank Pointers ($8000 - $FFFF) */
    uint8_t *rom_block[8];

    /* ID of the bank referenced by each ROM block */
    uint8_t rom_block_bank_id[8];

    /* Raw ROM data */
    uint8_t *rom;
    /* Bank ID of each 4k segment of the raw ROM data */
    uint8_t rom_bank_id[ROM_BANK_COUNT];
    /* Load status of a particular ROM bank */
    uint8_t rom_bank_loaded[ROM_BANK_COUNT];
    /* List of loaded banks in use order for LRU cache */
    int rom_bank_use_order[ROM_BANK_COUNT];

} nsf_nes_memory_t;

struct nsf_file_t {
    FILE *file;
    nsf_header_t header;
    nsf_nes_memory_t nes_memory;
    nsf_apu_write_cb_t apu_write_cb;
};

static nsf_file_t *active_nsf_file = NULL;

static esp_err_t nsf_read_header_impl(FILE *file, nsf_header_t *header);
static bool nsf_has_bank_switching(nsf_file_t *nsf);
static void IRAM_ATTR mark_rom_bank_used(nsf_nes_memory_t *nes_memory, uint8_t bank);
static void nsf_init_nes_memory(nsf_file_t *nsf);
static void nsf_init_nes_prg(nsf_file_t *nsf, uint8_t song, uint8_t pal_ntsc);
static esp_err_t nsf_init_load_nes_rom(nsf_file_t *nsf);
static esp_err_t nsf_init_load_nes_rom_banks(nsf_file_t *nsf);
static esp_err_t nsf_load_rom_bank(nsf_file_t *nsf, uint16_t reg, uint8_t bank);

esp_err_t nsf_read_header(const char *filename, nsf_header_t *header)
{
    int ret = ESP_OK;
    FILE *file = NULL;

    if (!filename || !header) {
        return ESP_ERR_INVALID_ARG;
    }

    do {
        file = fopen(filename, "rb");
        if (!file) {
            ESP_LOGE(TAG, "Failed to open file for reading: %s", strerror(errno));
            ret = ESP_FAIL;
            break;
        }

        ret = nsf_read_header_impl(file, header);
    } while (0);

    if (file) {
        fclose(file);
    }

    return ret;
}

esp_err_t nsf_open(nsf_file_t **nsf, const char *filename)
{
    int ret = 0;
    nsf_file_t *nsf_file = NULL;

    if (active_nsf_file) {
        ESP_LOGE(TAG, "Cannot have multiple NSF files open");
        return ESP_ERR_INVALID_STATE;
    }

    do {
        nsf_file = malloc(sizeof(struct nsf_file_t));
        if (!nsf) {
            ret = ESP_ERR_NO_MEM;
            break;
        }
        bzero(nsf_file, sizeof(struct nsf_file_t));

        nsf_file->file = fopen(filename, "rb");
        if (!nsf_file->file) {
            ESP_LOGE(TAG, "Failed to open file for reading: %s", strerror(errno));
            ret = ESP_FAIL;
            break;
        }

        ret = nsf_read_header_impl(nsf_file->file, &nsf_file->header);
        if (ret != ESP_OK) {
            break;
        }
    } while(0);

    if (ret >= 0) {
        *nsf = nsf_file;
        active_nsf_file = nsf_file;
    } else {
        nsf_free(nsf_file);
    }
    return ret;
}

esp_err_t nsf_read_header_impl(FILE *file, nsf_header_t *header)
{
    uint8_t buf[128];
    size_t n;

    n = fread(buf, 1, sizeof(buf), file);
    if (n != sizeof(buf)) {
        ESP_LOGE(TAG, "Short header");
        return -1;
    }

    if (memcmp(buf, "NESM\x1A", 5) != 0) {
        ESP_LOGE(TAG, "Invalid header start");
        return -1;
    }
    n = 5;

    header->version = buf[n++];
    header->total_songs = buf[n++];
    header->starting_song = buf[n++];

    header->load_address = buf[n] | (buf[n+1] << 8);
    n += 2;
    header->init_address = buf[n] | (buf[n+1] << 8);
    n += 2;
    header->play_address = buf[n] | (buf[n+1] << 8);
    n += 2;

    strncpy(header->name, (char *)(buf + n), 32);
    n += 32;
    strncpy(header->artist, (char *)(buf + n), 32);
    n += 32;
    strncpy(header->copyright, (char *)(buf + n), 32);
    n += 32;
    header->name[31] = '\0';
    header->artist[31] = '\0';
    header->copyright[31] = '\0';

    header->play_speed_ntsc = buf[n] | (buf[n+1] << 8);
    n += 2;

    memcpy(header->bankswitch_init, buf + n, 8);
    n += 8;

    header->play_speed_pal = buf[n] | (buf[n+1] << 8);
    n += 2;

    header->pal_ntsc_bits = buf[n++];
    header->extra_sound_chips = buf[n++];

    memcpy(header->extra, buf + n, 4);
    n += 4;

    assert(n == 0x80);

    return ESP_OK;
}

bool nsf_has_bank_switching(nsf_file_t *nsf)
{
    for (int i = 0; i < 8; i++) {
        if (nsf->header.bankswitch_init[i] != 0) {
            return true;
        }
    }
    return false;
}

void nsf_log_header_fields(const nsf_file_t *nsf)
{
    const nsf_header_t *header = &nsf->header;
    ESP_LOGI(TAG, "NSF File Header");
    ESP_LOGI(TAG, "---------------");
    ESP_LOGI(TAG, "Version: %d", header->version);
    ESP_LOGI(TAG, "Total songs: %d", header->total_songs);
    ESP_LOGI(TAG, "Starting song: %d", header->starting_song);
    ESP_LOGI(TAG, "Load address: $%04X", header->load_address);
    ESP_LOGI(TAG, "Init address: $%04X", header->init_address);
    ESP_LOGI(TAG, "Play address: $%04X", header->play_address);
    ESP_LOGI(TAG, "Name: \"%s\"", header->name);
    ESP_LOGI(TAG, "Artist: \"%s\"", header->artist);
    ESP_LOGI(TAG, "Copyright: \"%s\"", header->copyright);
    ESP_LOGI(TAG, "Play speed (NTSC): %d ticks", header->play_speed_ntsc);
    ESP_LOGI(TAG, "Play speed (PAL): %d ticks", header->play_speed_pal);
    ESP_LOGI(TAG, "Bankswitch: [%d][%d][%d][%d][%d][%d][%d][%d]",
            header->bankswitch_init[0], header->bankswitch_init[1],
            header->bankswitch_init[2], header->bankswitch_init[3],
            header->bankswitch_init[4], header->bankswitch_init[5],
            header->bankswitch_init[6], header->bankswitch_init[7]);

    if ((header->pal_ntsc_bits & 0xFC) != 0) {
        ESP_LOGI(TAG, "PAL/NTSC: invalid");
    } else if ((header->pal_ntsc_bits & 0x02) == 0x02) {
        ESP_LOGI(TAG, "PAL/NTSC: Dual PAL/NTSC");
    } else if ((header->pal_ntsc_bits & 0x01) == 0x01) {
        ESP_LOGI(TAG, "PAL/NTSC: PAL");
    } else {
        ESP_LOGI(TAG, "PAL/NTSC: NTSC");
    }

    ESP_LOGI(TAG, "Extra sound chips:");
    if ((header->extra_sound_chips & 0x01) == 0x01) {
        ESP_LOGI(TAG, " VRC6");
    }
    if ((header->extra_sound_chips & 0x02) == 0x02) {
        ESP_LOGI(TAG, " VRC7");
    }
    if ((header->extra_sound_chips & 0x04) == 0x04) {
        ESP_LOGI(TAG, " FDS");
    }
    if ((header->extra_sound_chips & 0x08) == 0x08) {
        ESP_LOGI(TAG, " MMC5");
    }
    if ((header->extra_sound_chips & 0x10) == 0x10) {
        ESP_LOGI(TAG, " Nameco_163");
    }
    if ((header->extra_sound_chips & 0x20) == 0x20) {
        ESP_LOGI(TAG, " Sunsoft_5B");
    }
    if ((header->extra_sound_chips & 0xC0) != 0) {
        ESP_LOGI(TAG, " Error");
    }

    ESP_LOGI(TAG, "Extra: %02X%02X%02X%02X",
            header->extra[0], header->extra[1],
            header->extra[2], header->extra[3]);
}

const nsf_header_t *nsf_get_header(const nsf_file_t *nsf)
{
    return &nsf->header;
}

void IRAM_ATTR mark_rom_bank_used(nsf_nes_memory_t *nes_memory, uint8_t bank)
{
    // Bank is already the most recent one
    if (nes_memory->rom_bank_use_order[0] == bank) {
        return;
    }
    else {
        int i;
        // Try to find the bank in the LRU collection, and shift it forward
        for (i = 1; i < ROM_BANK_COUNT; i++) {
            if (nes_memory->rom_bank_use_order[i] == bank) {
                for (int j = i; j > 0; --j) {
                    nes_memory->rom_bank_use_order[j] = nes_memory->rom_bank_use_order[j - 1];
                }
                // Insert the bank at the head
                nes_memory->rom_bank_use_order[0] = bank;
                return;
            }
        }

        // Bank was not in the collection, shift the list down and insert at the head
        ESP_LOGD(TAG, "Used ROM bank [%d] not in LRU collection!", bank);

        // Check for a condition that should not be possible if the loader code
        // is working correctly and evicting the oldest element before loading
        // a new one.
        if (nes_memory->rom_bank_use_order[ROM_BANK_COUNT - 1] != -1) {
            ESP_LOGE(TAG, "LRU collection tail was not empty!");
            return;
        }

        // Shift the use list down by one element
        for (i = ROM_BANK_COUNT - 1; i > 0; --i) {
            nes_memory->rom_bank_use_order[i] = nes_memory->rom_bank_use_order[i - 1];
        }
        // Insert the bank at the head
        nes_memory->rom_bank_use_order[0] = bank;
    }
}

uint8_t IRAM_ATTR read6502(uint16_t address)
{
    if (!active_nsf_file) { return 0; }
    nsf_nes_memory_t *nes_memory = &active_nsf_file->nes_memory;

    uint8_t value = 0;
    if (address <= 0x07FF) {
        value = nes_memory->ram[address];
    } else if (address >= 0x1000 && address <= 0x107F) {
        value = nes_memory->prg[address - 0x1000];
    } else if (address >= 0x4000 && address <= 0x4017) {
        value = nes_memory->apu_regs[address - 0x4000];
    } else if (address >= 0x5FF8 && address <= 0x5FFF) {
        value = nes_memory->bank_regs[address - 0x5FF8];
    } else if (address >= 0x8000 && address < 0xFFFA) {
        uint8_t block_index = (address & 0x7000) >> 12;
        if (nes_memory->rom_block[block_index] == 0) {
            ESP_LOGE(TAG, "Attempted read from unloaded block %d", block_index);
            return 0;
        }
        value = (nes_memory->rom_block[block_index])[address & 0x0FFF];
        mark_rom_bank_used(nes_memory, nes_memory->rom_block_bank_id[block_index]);
    } else if (address >= 0xFFFA) {
        value = nes_memory->int_vecs[address - 0xFFFA];
    }
    return value;
}

void IRAM_ATTR write6502(uint16_t address, uint8_t value)
{
    if (!active_nsf_file) { return; }
    nsf_nes_memory_t *nes_memory = &active_nsf_file->nes_memory;

    if (address <= 0x07FF) {
        nes_memory->ram[address] = value;
    } else if (address >= 0x4000 && address <= 0x4017) {
        nes_memory->apu_regs[address - 0x4000] = value;
        if (address != 0x4016) {
            //ESP_LOGI(TAG, "[%d] APU Write: $%04X <- $%02X\n",
            //        get6502_ticks(),
            //        address, value);
            if (active_nsf_file->apu_write_cb) {
                active_nsf_file->apu_write_cb(address, value);
            }
        }
    } else if (address >= 0x5FF8 && address <= 0x5FFF) {
        if (nes_memory->bank_regs[address - 0x5FF8] != value) {
            nes_memory->bank_regs[address - 0x5FF8] = value;
            nsf_load_rom_bank(active_nsf_file, address, value);
        }
    }
}

void nsf_init_nes_memory(nsf_file_t *nsf)
{
    nsf_nes_memory_t *nes_memory = &nsf->nes_memory;
    bzero(nes_memory, sizeof(nsf_nes_memory_t));
    nes_memory->apu_regs[0x17] = 0x40;
}

void nsf_init_nes_prg(nsf_file_t *nsf, uint8_t song, uint8_t pal_ntsc)
{
    int n = 0;
    nsf_nes_memory_t *nes_memory = &nsf->nes_memory;

    // LDA #song
    nes_memory->prg[n++] = 0xA9;
    nes_memory->prg[n++] = song;

    // LDX #pal_ntsc
    nes_memory->prg[n++] = 0xA2;
    nes_memory->prg[n++] = pal_ntsc;

    // JSR #init
    nes_memory->prg[n++] = 0x20;
    nes_memory->prg[n++] = (uint8_t)(nsf->header.init_address & 0x00FF);
    nes_memory->prg[n++] = (uint8_t)((nsf->header.init_address & 0xFF00) >> 8);

    // JSR #play
    nes_memory->prg[n++] = 0x20;
    nes_memory->prg[n++] = (uint8_t)(nsf->header.play_address & 0x00FF);
    nes_memory->prg[n++] = (uint8_t)((nsf->header.play_address & 0xFF00) >> 8);

    // JMP to previous
    nes_memory->prg[n++] = 0x4C;
    nes_memory->prg[n++] = 0x07;
    nes_memory->prg[n++] = 0x10;

    // NOP
    nes_memory->prg[n++] = 0xEA;
    nes_memory->prg[n++] = 0xEA;
    nes_memory->prg[n++] = 0xEA;
    nes_memory->prg[n++] = 0xEA;

    // Reset vector = 0x1000
    nes_memory->int_vecs[2] = 0x00;
    nes_memory->int_vecs[3] = 0x10;
}

esp_err_t nsf_init_load_nes_rom(nsf_file_t *nsf)
{
    if (nsf->header.load_address < 0x8000) {
        ESP_LOGE(TAG, "Bad load address: $%04X", nsf->header.load_address);
        return ESP_FAIL;
    }

    if (fseek(nsf->file, 0x080, SEEK_SET) < 0) {
        return ESP_FAIL;
    }

    int offset = nsf->header.load_address - 0x8000;
    int max_len = 0xFFFF - nsf->header.load_address;

    if (nsf->nes_memory.rom) {
        free(nsf->nes_memory.rom);
        nsf->nes_memory.rom = 0;
    }
    nsf->nes_memory.rom = malloc(32768);
    bzero(nsf->nes_memory.rom, 32768);
    bzero(nsf->nes_memory.rom_block, sizeof(nsf->nes_memory.rom_block));

    size_t n = fread(nsf->nes_memory.rom + offset, 1, max_len, nsf->file);

    if (n == 0) {
        ESP_LOGE(TAG, "Read error");
        return ESP_FAIL;
    } else if (n != max_len) {
        ESP_LOGW(TAG, "Short read: %d < %d", n, max_len);
    }

    for(int i = 0; i < 8; i++) {
        nsf->nes_memory.rom_block[i] = nsf->nes_memory.rom + (i * 4096);
    }

    return ESP_OK;
}

esp_err_t nsf_init_load_nes_rom_banks(nsf_file_t *nsf)
{
    esp_err_t ret = ESP_OK;

    if (nsf->nes_memory.rom) {
        free(nsf->nes_memory.rom);
        nsf->nes_memory.rom = 0;
    }
    nsf->nes_memory.rom = malloc(ROM_BANK_SIZE * ROM_BANK_COUNT);
    bzero(nsf->nes_memory.rom, ROM_BANK_SIZE * ROM_BANK_COUNT);
    bzero(nsf->nes_memory.rom_bank_id, ROM_BANK_COUNT);
    bzero(nsf->nes_memory.rom_bank_loaded, ROM_BANK_COUNT);

    for (int i = 0; i < ROM_BANK_COUNT; i++) {
        nsf->nes_memory.rom_bank_use_order[i] = -1;
    }

    bzero(nsf->nes_memory.rom_block, sizeof(nsf->nes_memory.rom_block));
    bzero(nsf->nes_memory.rom_block_bank_id, sizeof(nsf->nes_memory.rom_block_bank_id));

    for (int i = 0; i < 8; i++) {
        ret = nsf_load_rom_bank(nsf, 0x5FF8 + i, nsf->header.bankswitch_init[i]);
        if (ret != ESP_OK) {
            break;
        }
    }

    return ret;
}

esp_err_t nsf_load_rom_bank(nsf_file_t *nsf, uint16_t reg, uint8_t bank)
{
    if (reg < 0x5FF8 || reg > 0x5FFF) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGD(TAG, "Load bank: $%04X -> %d", reg, bank);

    int64_t time0 = esp_timer_get_time();
    nsf_nes_memory_t *nes_memory = &nsf->nes_memory;
    uint16_t padding = nsf->header.load_address & 0x0FFF;
    uint8_t target_block = reg - 0x5FF8;
    uint8_t i;

    // Check if bank is already loaded
    int bank_index = -1;
    for (i = 0; i < ROM_BANK_COUNT; i++) {
        if (nes_memory->rom_bank_loaded[i] && nes_memory->rom_bank_id[i] == bank) {
            bank_index = i;
            break;
        }
    }

    if (bank_index != -1) {
        // Bank is already loaded
        nes_memory->rom_block[target_block] = nes_memory->rom + (bank_index * 4096);
        nes_memory->rom_block_bank_id[target_block] = bank;
        mark_rom_bank_used(nes_memory, bank);
    } else {
        // Bank is not loaded

        // Find an empty bank slot
        bank_index = -1;
        for (i = 0; i < ROM_BANK_COUNT; i++) {
            if (!nes_memory->rom_bank_loaded[i]) {
                bank_index = i;
                break;
            }
        }
        if (bank_index == -1) {
            ESP_LOGD(TAG, "No empty ROM banks available");

            // Grab the ROM bank ID at the end of the use list
            int oldest_bank = nes_memory->rom_bank_use_order[ROM_BANK_COUNT - 1];
            if (oldest_bank == -1) {
                ESP_LOGE(TAG, "LRU list should not have an empty tail");
                return ESP_FAIL;
            }
            for (i = 0; i < ROM_BANK_COUNT; i++) {
                if (nes_memory->rom_bank_id[i] == oldest_bank) {
                    bank_index = i;
                    break;
                }
            }
            if (bank_index == -1) {
                ESP_LOGE(TAG, "Unable to find bank %d in loaded bank set!", oldest_bank);
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "Evicting bank %d from slot %d", nes_memory->rom_bank_id[bank_index], bank_index);
            // Set the least recently used bank as unloaded
            nes_memory->rom_bank_use_order[ROM_BANK_COUNT - 1] = -1;
            nes_memory->rom_bank_loaded[bank_index] = 0;
            nes_memory->rom_bank_id[bank_index] = 0;

            // If the unloaded bank is still referenced by a ROM block, then
            // that reference also needs to be cleared out.
            for (i = 0; i < 8; i++) {
                if (nes_memory->rom_block[i] && nes_memory->rom_block_bank_id[i] == bank_index) {
                    nes_memory->rom_block[i] = 0;
                    nes_memory->rom_block_bank_id[i] = 0;
                    break;
                }
            }
        }

        uint8_t *rom_bank = nes_memory->rom + (bank_index * 4096);

        // Clear the target bank
        bzero(rom_bank, 4096);
        nes_memory->rom_bank_loaded[bank_index] = 0;

        // Load the bank data
        if (bank == 0) {
            if (fseek(nsf->file, 0x080, SEEK_SET) < 0) {
                return ESP_FAIL;
            }

            size_t n = fread(rom_bank + padding, 1, 4096 - padding, nsf->file);
            if (n == 0 && !feof(nsf->file)) {
                ESP_LOGE(TAG, "Read error");
                return ESP_FAIL;
            }
        } else {
            if (fseek(nsf->file, 0x080 + (4096 - padding) + (4096 * (bank - 1)), SEEK_SET) < 0) {
                return ESP_FAIL;
            }

            size_t n = fread(rom_bank, 1, 4096, nsf->file);
            if (n == 0 && !feof(nsf->file)) {
                ESP_LOGE(TAG, "Read error");
                return ESP_FAIL;
            }
        }

        // Set the loaded state variables and pointers
        nes_memory->rom_bank_loaded[bank_index] = 1;
        nes_memory->rom_bank_id[bank_index] = bank;
        nes_memory->rom_block[target_block] = rom_bank;
        nes_memory->rom_block_bank_id[target_block] = bank;
        mark_rom_bank_used(nes_memory, bank);

        int64_t time1 = esp_timer_get_time();
        ESP_LOGI(TAG, "Bank loaded: $%04X -> %d [%lld(us)]", reg, bank, (time1-time0));
    }

    return ESP_OK;
}

esp_err_t nsf_playback_init(nsf_file_t *nsf, uint8_t song, nsf_apu_write_cb_t apu_write_cb)
{
    esp_err_t ret;
    nsf->apu_write_cb = apu_write_cb;
    nsf_init_nes_memory(nsf);
    nsf_init_nes_prg(nsf, song, 0);

    if (nsf_has_bank_switching(nsf)) {
        ESP_LOGI(TAG, "Playback init loading bankswitched ROM");
        ret = nsf_init_load_nes_rom_banks(nsf);
    } else {
        ESP_LOGI(TAG, "Playback init loading contiguous ROM");
        ret = nsf_init_load_nes_rom(nsf);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    reset6502();

    do {
        step6502();
        //TODO have a sanity check condition
    } while (get6502_pc() != 0x1007);

    return ESP_OK;
}

esp_err_t nsf_playback_frame(nsf_file_t *nsf)
{
    if (get6502_pc() != 0x1007) {
        return ESP_ERR_INVALID_STATE;
    }

    do {
        step6502();
        //TODO have a sanity check condition
    } while (get6502_pc() != 0x1007);

    return ESP_OK;
}

void nsf_free(nsf_file_t *nsf)
{
    if (nsf) {
        assert(active_nsf_file == nsf);
        if (nsf->file) {
            fclose(nsf->file);
        }
        if (nsf->nes_memory.rom) {
            free(nsf->nes_memory.rom);
        }
        free(nsf);
        active_nsf_file = NULL;
    }
}
