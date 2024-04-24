#include "file_reader.h"

struct clusters_chain_t *get_chain_fat16(const void *const buffer, size_t size, uint16_t first_cluster) {
    if (!buffer || size == 0) {
        return NULL;
    }

    int max_uint16 = (int) size / 2;
    uint8_t *temp = (uint8_t *) buffer;
    if (first_cluster > max_uint16) {
        return NULL;
    }
    uint16_t first, second, result;

    struct clusters_chain_t *clustersChain = malloc(sizeof(struct clusters_chain_t));
    if (!clustersChain) {
        return NULL;
    }
    clustersChain->size = 0;
    clustersChain->clusters = NULL;

    while (1) {
        first = temp[first_cluster * 2];
        second = temp[first_cluster * 2 + 1];
        result = (second << 8) | first;
        clustersChain->size++;
        uint16_t *var = realloc(clustersChain->clusters, clustersChain->size * sizeof(uint16_t));
        if (!var) {
            free(clustersChain->clusters);
            free(clustersChain);
            return NULL;
        }
        clustersChain->clusters = var;
        clustersChain->clusters[clustersChain->size - 1] = first_cluster;
        if (result >= 0xFFF0) {
            break;
        }
        first_cluster = result;
        if (first_cluster >= max_uint16) {
            free(clustersChain->clusters);
            free(clustersChain);
            return NULL;
        }
    }

    return clustersChain;
}

struct disk_t *disk_open_from_file(const char *volume_file_name) {
    if (volume_file_name == NULL) {
        errno = EFAULT;
        return NULL;
    }
    struct disk_t *disk = malloc(sizeof(struct disk_t));
    if (disk == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    disk->disk = fopen(volume_file_name, "rb");
    if (disk->disk == NULL) {
        errno = ENOENT;
        free(disk);
        return NULL;
    }
    return disk;
}

int disk_read(struct disk_t *pdisk, int32_t first_sector, void *buffer, int32_t sectors_to_read) {
    if (pdisk == NULL || pdisk->disk == NULL || buffer == NULL) {
        errno = EFAULT;
        return -1;
    }
    fseek(pdisk->disk, 0, SEEK_END);
    if (sectors_to_read + first_sector > ftell(pdisk->disk) / SECTOR_SIZE || first_sector < 0 || sectors_to_read < 1) {
        errno = ERANGE;
        return -1;
    }
    fseek(pdisk->disk, first_sector * SECTOR_SIZE, SEEK_SET);
    return (int) fread(buffer, SECTOR_SIZE, sectors_to_read, pdisk->disk);
}

int disk_close(struct disk_t *pdisk) {
    if (pdisk == NULL || pdisk->disk == NULL) {
        errno = EFAULT;
        return -1;
    }
    fclose(pdisk->disk);
    free(pdisk);
    return 0;
}

struct volume_t *fat_open(struct disk_t *pdisk, uint32_t first_sector) {
    if (pdisk == NULL || pdisk->disk == NULL) {
        errno = EFAULT;
        return NULL;
    }

    struct volume_t *volume = malloc(sizeof(struct volume_t));
    if (volume == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    if (disk_read(pdisk, (int32_t) first_sector, volume, 1) != 1) {
        free(volume);
        return NULL;
    }

    int error = 1;
    for (int i = 1; i <= 128; i = i * 2) {
        if (volume->super.sectors_per_clusters == i) {
            error = 0;
        }
    }
    if (error) {
        free(volume);
        errno = EINVAL;
        return NULL;
    }

    if (volume->super.number_of_fats != 1 && volume->super.number_of_fats != 2) {
        free(volume);
        errno = EINVAL;
        return NULL;
    }
    uint8_t *fat_1 = malloc(volume->super.size_of_fat * volume->super.bytes_per_sector);
    if (fat_1 == NULL) {
        free(volume);
        errno = ENOMEM;
        return NULL;
    }
    volume->fat_1_position = volume->super.size_of_reserved_area;
    if (disk_read(pdisk, volume->fat_1_position, fat_1, volume->super.size_of_fat) != volume->super.size_of_fat) {
        free(volume);
        free(fat_1);
        errno = EINVAL;
        return NULL;
    }
    volume->disk = pdisk;
    volume->root_directory_position = volume->fat_1_position + volume->super.size_of_fat;
    if (volume->super.number_of_fats == 2) {
        uint8_t *fat_2 = malloc(volume->super.size_of_fat * volume->super.bytes_per_sector);
        if (fat_2 == NULL) {
            free(volume);
            free(fat_1);
            errno = ENOMEM;
            return NULL;
        }
        if (disk_read(pdisk, volume->fat_1_position + volume->super.size_of_fat, fat_2, volume->super.size_of_fat) !=
            volume->super.size_of_fat) {
            free(fat_1);
            free(fat_2);
            free(volume);
            errno = EINVAL;
            return NULL;
        }
        if (memcmp(fat_1, fat_2, volume->super.size_of_fat * volume->super.bytes_per_sector) != 0) {
            free(fat_1);
            free(fat_2);
            free(volume);
            errno = EINVAL;
            return NULL;
        }
        free(fat_2);
        volume->root_directory_position += volume->super.size_of_fat;
    }
    volume->fat = fat_1;
    volume->data_start = volume->super.size_of_reserved_area + volume->super.number_of_sectors_before_partition +
                         (volume->super.number_of_fats * volume->super.size_of_fat) +
                         (volume->super.maximum_number_of_files / 16);
    return volume;
}

int fat_close(struct volume_t *pvolume) {
    if (pvolume == NULL) {
        errno = EFAULT;
        return -1;
    }
    free(pvolume->fat);
    free(pvolume);
    return 0;
}

struct file_t *file_open(struct volume_t *pvolume, const char *file_name) {
    if (pvolume == NULL || file_name == NULL) {
        errno = EFAULT;
        return NULL;
    }
    char *upper_path = malloc(strlen(file_name) + 1);
    if (upper_path == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    upper_path[strlen(file_name)] = '\0';
    int max_dirs = 2;
    for (int i = 0; file_name[i] != '\0'; i++) {
        upper_path[i] = (char) toupper(file_name[i]);
        if (i != 0 && upper_path[i] == '\\') {
            max_dirs++;
        }
    }
    struct file_t *file = malloc(sizeof(struct file_t));
    if (file == NULL) {
        errno = ENOMEM;
        free(upper_path);
        return NULL;
    }
    uint8_t *boot_record = malloc(sizeof(struct SFN) * pvolume->super.maximum_number_of_files);
    if (boot_record == NULL) {
        free(file);
        free(upper_path);
        errno = ENOMEM;
        return NULL;
    }
    if (disk_read(pvolume->disk, pvolume->root_directory_position, boot_record,
                  (int32_t) sizeof(struct SFN) * pvolume->super.maximum_number_of_files /
                  pvolume->super.bytes_per_sector) !=
        (int) (sizeof(struct SFN) * pvolume->super.maximum_number_of_files / pvolume->super.bytes_per_sector)) {
        free(file);
        free(boot_record);
        free(upper_path);
        // errno ?????
        return NULL;
    }
    file->volume = pvolume;
    file->offset = 0;
    file->entry = NULL;
    struct SFN **dirs = malloc(sizeof(struct SFN *) * max_dirs);
    if (dirs == NULL) {
        free(file);
        free(boot_record);
        free(upper_path);
        errno = ENOMEM;
        return NULL;
    }
    int path_index = upper_path[0] == '\\' ? 1 : 0;
    int current_dir_index = 0;
    *dirs = (struct SFN *) boot_record;
    char *expected_upper_name = NULL;
    for (int i = 0; i < max_dirs - 1; i++) {
        int temp = 0;
        for (; upper_path[path_index] != '\0'; path_index++) {
            expected_upper_name = realloc(expected_upper_name, temp + 2);
            if (upper_path[path_index] == '\\') {
                path_index++;
                break;
            }
            expected_upper_name[temp] = upper_path[path_index];
            expected_upper_name[temp + 1] = '\0';
            temp++;
        }
        if (strcmp(expected_upper_name, ".") == 0) {
            continue;
        } else if (strcmp(expected_upper_name, "..") == 0) {
            if (current_dir_index == 0) {
                errno = ENOENT;
                free(file);
                free(upper_path);
                free(expected_upper_name);
                for (int a = 0; a <= current_dir_index; a++) {
                    free(dirs[a]);
                }
                free(dirs);
                return NULL;
            }
            free(dirs[current_dir_index]);
            current_dir_index--;
            continue;
        } else {
            int found = 0;
            int is_lfn = 0;
            for (int j = 0;; j++) {
                char *name = calloc(1, 13);
                temp = 0;
                if (dirs[current_dir_index][j].file_attributes == 0x0F) {
                    is_lfn = 1;
                    free(name);
                    continue;
                }
                if (dirs[current_dir_index][j].filename[0] == 0x00) {
                    free(name);
                    break;
                }
                if (!is_lfn) {
                    struct SFN *curr = dirs[current_dir_index] + j;
                    for (int k = 0; k < 8; k++) {
                        char letter = curr->filename[k];
                        if (isprint(letter) && letter != ' ') {
                            name[temp] = letter;
                            temp++;
                        }
                    }
                    for (int k = 8; k < 11; k++) {
                        char letter = dirs[current_dir_index][j].filename[k];
                        if (isprint(letter) && letter != ' ') {
                            if (k == 8) {
                                name[temp] = '.';
                                temp++;
                            }
                            name[temp] = letter;
                            temp++;
                        }
                    }
                } else {
                    for (int k = 1;; k++) {
                        struct SFN *mainn = dirs[current_dir_index] + j;
                        char p = mainn->filename[0];
                        if (p == '\0') {
                            break;
                        }
                        struct LFN *curr = (struct LFN *) (dirs[current_dir_index] + (j - k));
                        for (int l = 0; l < 10; l++) {
                            char letter = ((struct LFN *) (curr))->filename1[l];
                            if (isprint(letter)) {
                                name = realloc(name, temp + 2);
                                name[temp] = toupper(letter);
                                name[temp + 1] = '\0';
                                temp++;
                            }
                        }
                        for (int l = 0; l < 12; l++) {
                            char letter = ((struct LFN *) (dirs[current_dir_index] + (j - k)))->filename2[l];
                            if (isprint(letter)) {
                                name = realloc(name, temp + 2);
                                name[temp] = toupper(letter);
                                name[temp + 1] = '\0';
                                temp++;
                            }
                        }
                        for (int l = 0; l < 4; l++) {
                            char letter = ((struct LFN *) (dirs[current_dir_index] + (j - k)))->filename3[l];
                            if (isprint(letter)) {
                                name = realloc(name, temp + 2);
                                name[temp] = toupper(letter);
                                name[temp + 1] = '\0';
                                temp++;
                            }
                        }
                        if (((struct LFN *) (dirs[current_dir_index] + (j - k)))->sequence_number >= 0x40) {
                            is_lfn = 0;
                            break;
                        }
                    }

                }
                if (strcmp(expected_upper_name, name) == 0) {
                    if (dirs[current_dir_index][j].file_attributes & 0x08) {
                        errno = ENOTDIR;
                        free(file);
                        free(upper_path);
                        free(name);
                        free(expected_upper_name);
                        for (int a = 0; a <= current_dir_index; a++) {
                            free(dirs[a]);
                        }
                        free(dirs);
                        return NULL;
                    } else if ((dirs[current_dir_index][j].file_attributes & 0x10) == 0) {
                        if (i == max_dirs - 2) {
                            found = 1;
                            file->clusters = get_chain_fat16(pvolume->fat, pvolume->super.size_of_fat *
                                                                           pvolume->super.bytes_per_sector,
                                                             dirs[current_dir_index][j].low_order_address_of_first_cluster);
                            file->entry = malloc(sizeof(struct SFN));
                            memcpy(file->entry, dirs[current_dir_index] + j, sizeof(struct SFN));
                            is_lfn = 0;
                            free(name);
                            break;
                        } else {
                            errno = ENOTDIR;
                            free(file);
                            free(upper_path);
                            free(name);
                            free(expected_upper_name);
                            for (int a = 0; a <= current_dir_index; a++) {
                                free(dirs[a]);
                            }
                            free(dirs);
                            return NULL;
                        }
                    } else {
                        found = 1;
                        struct clusters_chain_t *clustersChain = get_chain_fat16(pvolume->fat,
                                                                                 pvolume->super.size_of_fat *
                                                                                 pvolume->super.bytes_per_sector,
                                                                                 dirs[current_dir_index][j].low_order_address_of_first_cluster);
                        dirs[current_dir_index + 1] = malloc(
                                clustersChain->size * SECTOR_SIZE * pvolume->super.sectors_per_clusters);
                        if (dirs[current_dir_index + 1] == NULL) {
                            errno = ENOMEM;
                            free(file);
                            free(name);
                            free(upper_path);
                            free(expected_upper_name);
                            free(clustersChain->clusters);
                            free(clustersChain);
                            for (int a = 0; a <= current_dir_index; a++) {
                                free(dirs[a]);
                            }
                            free(dirs);
                            return NULL;
                        }
                        uint32_t read = 0;
                        uint8_t *buff = malloc(SECTOR_SIZE * pvolume->super.sectors_per_clusters);
                        for (size_t k = 0; k < clustersChain->size; k++) {
                            if (disk_read(pvolume->disk, pvolume->data_start + pvolume->super.sectors_per_clusters *
                                                                               (clustersChain->clusters[k] - 2), buff,
                                          pvolume->super.sectors_per_clusters) != pvolume->super.sectors_per_clusters) {
                                errno = ERANGE;
                                free(file);
                                free(name);
                                free(expected_upper_name);
                                free(clustersChain->clusters);
                                free(clustersChain);
                                free(upper_path);
                                for (int a = 0; a <= current_dir_index; a++) {
                                    free(dirs[a]);
                                }
                                free(dirs);
                                return NULL;
                            }
                            memcpy((uint8_t *) (dirs[current_dir_index + 1]) + read, buff,
                                   pvolume->super.sectors_per_clusters * SECTOR_SIZE);
                            read += pvolume->super.sectors_per_clusters * SECTOR_SIZE;
                        }
                        free(buff);
                        free(clustersChain->clusters);
                        free(clustersChain);
                        current_dir_index++;
                        is_lfn = 0;
                        free(name);
                        break;
                    }
                }
                free(name);
            }
            if (!found) {
                errno = ENOENT;
                free(file);
                free(expected_upper_name);
                free(upper_path);
                for (int a = 0; a <= current_dir_index; a++) {
                    free(dirs[a]);
                }
                free(dirs);
                return NULL;
            }

        }
    }
    for (int i = 0; i <= current_dir_index; i++) {
        free(dirs[i]);
    }
    free(upper_path);
    free(expected_upper_name);
    free(dirs);
    if (file->entry == NULL) {
        errno = EISDIR;
        free(file);
        return NULL;
    }
    return file;
}

int file_close(struct file_t *stream) {
    if (stream == NULL) {
        errno = EFAULT;
        return -1;
    }
    free(stream->entry);
    free(stream->clusters->clusters);
    free(stream->clusters);
    free(stream);
    return 0;
}

int32_t file_seek(struct file_t *stream, int32_t offset, int whence) {
    if (stream == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (whence == SEEK_SET) {
        if ((uint32_t) offset > stream->entry->size || offset < 0) {
            errno = ENXIO;
            return -1;
        }
        stream->offset = offset;
        return (int32_t) stream->offset;
    } else if (whence == SEEK_CUR) {
        if (offset + (int32_t) stream->offset > (int32_t) stream->entry->size ||
            offset + (int32_t) stream->offset < 0) {
            errno = ENXIO;
            return -1;
        }
        stream->offset += offset;
        return (int32_t) stream->offset;
    } else if (whence == SEEK_END) {
        if (stream->entry->size + offset > stream->entry->size) {
            errno = ENXIO;
            return -1;
        }
        stream->offset = stream->entry->size + offset;
        return (int32_t) stream->offset;
    }
    errno = EINVAL;
    return -1;
}

size_t file_read(void *ptr, size_t size, size_t nmemb, struct file_t *stream) {
    if (ptr == NULL || stream == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (stream->offset >= stream->entry->size) {
        // errno = ?????
        return 0;
    }
    size_t read = 0;
    uint8_t *result = ptr;
    uint8_t *temp = malloc(stream->volume->super.sectors_per_clusters * SECTOR_SIZE);
    if (temp == NULL) {
        errno = ENOMEM;
        return 0;
    }
    if (stream->offset % (SECTOR_SIZE * stream->volume->super.sectors_per_clusters) != 0) {
        if (disk_read(stream->volume->disk, stream->volume->data_start + stream->volume->super.sectors_per_clusters *
                                                                         (stream->clusters->clusters[(int) (
                                                                                 stream->offset / (SECTOR_SIZE *
                                                                                                   stream->volume->super.sectors_per_clusters))] -
                                                                          2), temp,
                      stream->volume->super.sectors_per_clusters) == 0) {
            errno = ERANGE;
            free(temp);
            return 0;
        }
        uint16_t to_read = (SECTOR_SIZE * stream->volume->super.sectors_per_clusters) -
                           stream->offset % (SECTOR_SIZE * stream->volume->super.sectors_per_clusters);
        if (to_read > size * nmemb) {
            for (int j = 0;; j++) {
                if (read == size * nmemb) {
                    free(temp);
                    stream->offset += nmemb * size;
                    return nmemb;
                } else if (stream->offset + read == stream->entry->size) {
                    free(temp);
                    stream->offset += (size_t) (read / size) * size;
                    return (size_t) (read / size);
                }
                memcpy(result + read,
                       temp + j + (stream->offset % (SECTOR_SIZE * stream->volume->super.sectors_per_clusters)), 1);
                read++;
            }
        } else {
            memcpy(result, temp + stream->offset % (SECTOR_SIZE * stream->volume->super.sectors_per_clusters), to_read);
            read += to_read;
        }
    }
    for (int i = (int) ((stream->offset + read) / (SECTOR_SIZE * stream->volume->super.sectors_per_clusters));; i++) {
        if (read == size * nmemb || stream->offset + read > stream->entry->size) {
            break;
        }
        if (disk_read(stream->volume->disk, stream->volume->data_start + stream->volume->super.sectors_per_clusters *
                                                                         (stream->clusters->clusters[i] - 2), temp,
                      stream->volume->super.sectors_per_clusters) == 0) {
            errno = ERANGE;
            free(temp);
            return 0;
        }
        if (read + (SECTOR_SIZE * stream->volume->super.sectors_per_clusters) > size * nmemb ||
            read + (SECTOR_SIZE * stream->volume->super.sectors_per_clusters) > stream->entry->size) {
            for (int j = 0;; j++) {
                if (read == size * nmemb) {
                    free(temp);
                    stream->offset += nmemb * size;
                    return nmemb;
                } else if (stream->offset + read == stream->entry->size) {
                    free(temp);
                    stream->offset += (size_t) (read / size) * size;
                    return (size_t) (read / size);
                }
                memcpy(result + read, temp + j, 1);
                read++;
            }
        } else {
            memcpy(result + read, temp, SECTOR_SIZE * stream->volume->super.sectors_per_clusters);
            read += SECTOR_SIZE * stream->volume->super.sectors_per_clusters;
        }
    }
    free(temp);
    stream->offset += (size_t) (read / size) * size;
    return (size_t) (read / size);
}

struct dir_t *dir_open(struct volume_t *pvolume, const char *dir_path) {
    if (pvolume == NULL || dir_path == NULL) {
        errno = EFAULT;
        return NULL;
    }
    char *upper_dir_path = malloc(strlen(dir_path) + 1);
    if (upper_dir_path == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    upper_dir_path[strlen(dir_path)] = '\0';
    int max_dirs = 2;
    for (int i = 0; dir_path[i] != '\0'; i++) {
        upper_dir_path[i] = (char) toupper(dir_path[i]);
        if (i != 0 && upper_dir_path[i] == '\\') {
            max_dirs++;
        }
    }
    struct dir_t *dir = malloc(sizeof(struct dir_t));
    if (dir == NULL) {
        errno = ENOMEM;
        free(upper_dir_path);
        return NULL;
    }
    uint8_t *boot_record = malloc(sizeof(struct SFN) * pvolume->super.maximum_number_of_files);
    if (boot_record == NULL) {
        free(dir);
        free(upper_dir_path);
        errno = ENOMEM;
        return NULL;
    }
    if (disk_read(pvolume->disk, pvolume->root_directory_position, boot_record,
                  (int32_t) sizeof(struct SFN) * pvolume->super.maximum_number_of_files /
                  pvolume->super.bytes_per_sector) !=
        (int) (sizeof(struct SFN) * pvolume->super.maximum_number_of_files / pvolume->super.bytes_per_sector)) {
        free(dir);
        free(boot_record);
        free(upper_dir_path);
        return NULL;
    }
    dir->lfn_count = 0;
    dir->lfn = NULL;
    dir->volume = pvolume;
    dir->offset = 0;
    if (strcmp(dir_path, "\\") == 0) {
        free(upper_dir_path);
        dir->entry = (struct SFN *) boot_record;
        dir->offset = 1;
        return dir;
    }
    struct SFN firsts[2];
    struct SFN **dirs = malloc(sizeof(struct SFN *) * max_dirs);
    if (dirs == NULL) {
        free(dir);
        free(boot_record);
        free(upper_dir_path);
        errno = ENOMEM;
        return NULL;
    }
    int path_index = upper_dir_path[0] == '\\' ? 1 : 0;
    int current_dir_index = 0;
    char *expected_upper_name = NULL;
    *dirs = (struct SFN *) boot_record;
    for (int i = 0; i < max_dirs - 1; i++) {
        int temp = 0;
        for (; upper_dir_path[path_index] != '\0'; path_index++) {
            expected_upper_name = realloc(expected_upper_name, temp + 2);
            if (upper_dir_path[path_index] == '\\') {
                path_index++;
                break;
            }
            expected_upper_name[temp] = upper_dir_path[path_index];
            expected_upper_name[temp + 1] = '\0';
            temp++;
        }
        if (strcmp(expected_upper_name, ".") == 0) {
            continue;
        } else if (strcmp(expected_upper_name, "..") == 0) {
            if (current_dir_index == 0) {
                errno = ENOENT;
                free(dir);
                free(upper_dir_path);
                for (int a = 0; a <= current_dir_index; a++) {
                    free(dirs[a]);
                }
                free(dirs);
                return NULL;
            }
            free(dirs[current_dir_index]);
            current_dir_index--;
            continue;
        } else {
            int found = 0;
            int is_lfn = 0;
            for (int j = 0;; j++) {
                char *name = calloc(1, 13);
                temp = 0;
                if (dirs[current_dir_index][j].file_attributes == 0x0F) {
                    is_lfn = 1;
                    free(name);
                    continue;
                }
                if (dirs[current_dir_index][j].filename[0] == 0x00) {
                    free(name);
                    break;
                }
                if (!is_lfn) {
                    struct SFN *curr = dirs[current_dir_index] + j;
                    for (int k = 0; k < 8; k++) {
                        char letter = curr->filename[k];
                        if (isprint(letter) && letter != ' ') {
                            name[temp] = letter;
                            temp++;
                        }
                    }
                    for (int k = 8; k < 11; k++) {
                        char letter = dirs[current_dir_index][j].filename[k];
                        if (isprint(letter) && letter != ' ') {
                            if (k == 8) {
                                name[temp] = '.';
                                temp++;
                            }
                            name[temp] = letter;
                            temp++;
                        }
                    }
                } else {
                    for (int k = 1;; k++) {
                        struct SFN *mainn = dirs[current_dir_index] + j;
                        char p = mainn->filename[0];
                        if (p == '\0') {
                            break;
                        }
                        struct LFN *curr = (struct LFN *) (dirs[current_dir_index] + (j - k));
                        for (int l = 0; l < 10; l++) {
                            char letter = ((struct LFN *) (curr))->filename1[l];
                            if (isprint(letter)) {
                                name = realloc(name, temp + 2);
                                name[temp] = toupper(letter);
                                name[temp + 1] = '\0';
                                temp++;
                            }
                        }
                        for (int l = 0; l < 12; l++) {
                            char letter = ((struct LFN *) (dirs[current_dir_index] + (j - k)))->filename2[l];
                            if (isprint(letter)) {
                                name = realloc(name, temp + 2);
                                name[temp] = toupper(letter);
                                name[temp + 1] = '\0';
                                temp++;
                            }
                        }
                        for (int l = 0; l < 4; l++) {
                            char letter = ((struct LFN *) (dirs[current_dir_index] + (j - k)))->filename3[l];
                            if (isprint(letter)) {
                                name = realloc(name, temp + 2);
                                name[temp] = toupper(letter);
                                name[temp + 1] = '\0';
                                temp++;
                            }
                        }
                        if (((struct LFN *) (dirs[current_dir_index] + (j - k)))->sequence_number >= 0x40) {
                            is_lfn = 0;
                            break;
                        }
                    }

                }
                if (strcmp(expected_upper_name, name) == 0) {
                    if ((dirs[current_dir_index][j].file_attributes & 0x10) == 0 ||
                        dirs[current_dir_index][j].file_attributes & 0x08) {
                        errno = ENOTDIR;
                        free(dir);
                        free(upper_dir_path);
                        free(name);
                        free(expected_upper_name);
                        for (int a = 0; a <= current_dir_index; a++) {
                            free(dirs[a]);
                        }
                        free(dirs);
                        return NULL;
                    } else {
                        found = 1;
                        struct clusters_chain_t *clustersChain = get_chain_fat16(pvolume->fat,
                                                                                 pvolume->super.size_of_fat *
                                                                                 pvolume->super.bytes_per_sector,
                                                                                 dirs[current_dir_index][j].low_order_address_of_first_cluster);
                        dirs[current_dir_index + 1] = malloc(
                                clustersChain->size * SECTOR_SIZE * pvolume->super.sectors_per_clusters);
                        if (dirs[current_dir_index + 1] == NULL) {
                            errno = ENOMEM;
                            free(dir);
                            free(upper_dir_path);
                            free(name);
                            free(expected_upper_name);
                            free(clustersChain->clusters);
                            free(clustersChain);
                            for (int a = 0; a <= current_dir_index; a++) {
                                free(dirs[a]);
                            }
                            free(dirs);
                            return NULL;
                        }
                        uint32_t read = 0;
                        uint8_t *buff = malloc(SECTOR_SIZE * pvolume->super.sectors_per_clusters);
                        for (size_t k = 0; k < clustersChain->size; k++) {
                            if (disk_read(pvolume->disk, pvolume->data_start + pvolume->super.sectors_per_clusters *
                                                                               (clustersChain->clusters[k] - 2), buff,
                                          pvolume->super.sectors_per_clusters) != pvolume->super.sectors_per_clusters) {
                                errno = ERANGE;
                                free(dir);
                                free(clustersChain->clusters);
                                free(clustersChain);
                                free(upper_dir_path);
                                free(name);
                                free(expected_upper_name);
                                for (int a = 0; a <= current_dir_index; a++) {
                                    free(dirs[a]);
                                }
                                free(dirs);
                                return NULL;
                            }
                            if (k == 0) {
                                memcpy((uint8_t *) (dirs[current_dir_index + 1]) + read,
                                       buff + (sizeof(struct SFN) * 2),
                                       pvolume->super.sectors_per_clusters * SECTOR_SIZE - sizeof(struct SFN) * 2);
                                read += pvolume->super.sectors_per_clusters * SECTOR_SIZE - sizeof(struct SFN) * 2;
                                memcpy(firsts, buff, sizeof(struct SFN) * 2);
                            } else {
                                memcpy((uint8_t *) (dirs[current_dir_index + 1]) + read, buff,
                                       pvolume->super.sectors_per_clusters * SECTOR_SIZE);
                                read += pvolume->super.sectors_per_clusters * SECTOR_SIZE;
                            }
                        }
                        free(buff);
                        free(clustersChain->clusters);
                        free(clustersChain);
                        current_dir_index++;
                        is_lfn = 0;
                        free(name);
                        break;
                    }
                }
                free(name);
            }
            if (!found) {
                errno = ENOENT;
                free(dir);
                free(expected_upper_name);
                free(upper_dir_path);
                for (int a = 0; a <= current_dir_index; a++) {
                    free(dirs[a]);
                }
                free(dirs);
                return NULL;
            }

        }
    }
    dir->entry = dirs[current_dir_index];
    for (int i = 0;; i++) {
        if (dir->entry[i].filename[0] == 0x00) {
            memcpy(dir->entry + i, firsts, sizeof(struct SFN) * 2);
            break;
        }
    }
    for (int i = 0; i < current_dir_index; i++) {
        free(dirs[i]);
    }
    free(expected_upper_name);
    free(upper_dir_path);
    free(dirs);
    return dir;
}

int dir_read(struct dir_t *pdir, struct dir_entry_t *pentry) {
    if (pdir == NULL || pentry == NULL) {
        errno = EFAULT;
        return -1;
    }
    int is_lfn = 0;
    for (uint32_t i = pdir->offset; i < pdir->volume->super.maximum_number_of_files; i++) {
        char *name = calloc(1, 13);
        pdir->offset++;
        if (pdir->entry[i].filename[0] == 0x00) {
            free(name);
            break;
        }
        if (pdir->entry[i].filename[0] == (char) 0xE5 || pdir->entry[i].filename[0] == (char) 0x05) {
            free(name);
            continue;
        }
        if (pdir->entry[i].file_attributes == 0x0F) {
            is_lfn = 1;
            free(name);
            continue;
        }
        int temp = 0;
        if (!is_lfn) {
            struct SFN *curr = pdir->entry + i;
            for (int k = 0; k < 8; k++) {
                char letter = curr->filename[k];
                if (isprint(letter) && letter != ' ') {
                    name[temp] = letter;
                    temp++;
                }
            }
            for (int k = 8; k < 11; k++) {
                char letter = pdir->entry[i].filename[k];
                if (isprint(letter) && letter != ' ') {
                    if (k == 8) {
                        name[temp] = '.';
                        temp++;
                    }
                    name[temp] = letter;
                    temp++;
                }
            }
        } else {
            for (int k = 1;; k++) {
                struct SFN *mainn = pdir->entry + i;
                char p = mainn->filename[0];
                if (p == '\0') {
                    break;
                }
                struct LFN *curr = (struct LFN *) (pdir->entry + (i - k));
                for (int l = 0; l < 10; l++) {
                    char letter = ((struct LFN *) (curr))->filename1[l];
                    if (isprint(letter)) {
                        name = realloc(name, temp + 2);
                        name[temp] = letter;
                        name[temp + 1] = '\0';
                        temp++;
                    }
                }
                for (int l = 0; l < 12; l++) {
                    char letter = ((struct LFN *) (pdir->entry + (i - k)))->filename2[l];
                    if (isprint(letter)) {
                        name = realloc(name, temp + 2);
                        name[temp] = letter;
                        name[temp + 1] = '\0';
                        temp++;
                    }
                }
                for (int l = 0; l < 4; l++) {
                    char letter = ((struct LFN *) (pdir->entry + (i - k)))->filename3[l];
                    if (isprint(letter)) {
                        name = realloc(name, temp + 2);
                        name[temp] = letter;
                        name[temp + 1] = '\0';
                        temp++;
                    }
                }
                if (((struct LFN *) (pdir->entry + (i - k)))->sequence_number >= 0x40) {
                    break;
                }
            }
        }
        memcpy(pentry->name, name, 13);
        pentry->size = pdir->entry[i].size;
        pentry->is_readonly = ((pdir->entry[i].file_attributes >> 0) & 1);
        pentry->is_hidden = ((pdir->entry[i].file_attributes >> 1) & 1);
        pentry->is_system = ((pdir->entry[i].file_attributes >> 2) & 1);
        pentry->is_archived = ((pdir->entry[i].file_attributes >> 5) & 1);
        pentry->is_directory = pdir->entry[i].size == 0;
        if (is_lfn) {
            pentry->has_long_name = true;
            pentry->long_name = name;
            pdir->lfn_count++;
            pdir->lfn = realloc(pdir->lfn, sizeof(char *) * pdir->lfn_count);
            pdir->lfn[pdir->lfn_count - 1] = name;
        } else {
            pentry->has_long_name = false;
            pentry->long_name = NULL;
            free(name);
        }
        return 0;
    }
    return 1;
}

int dir_close(struct dir_t *pdir) {
    if (pdir == NULL) {
        errno = EFAULT;
        return -1;
    }
    free(pdir->entry);
    for (int i = 0; i < pdir->lfn_count; i++) {
        free(pdir->lfn[i]);
    }
    free(pdir->lfn);
    free(pdir);
    return 0;
}
